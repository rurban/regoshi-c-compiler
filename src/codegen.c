// SPDX-License-Identifier: LGPL-2.1-or-later
// Derived from chibicc by Rui Ueyama.
// Binary codegen: asm_* wrappers emit bytes via secbuf_emit*() to ObjFile.
#include "rcc.h"

#include "codegen_asm.h"
#include <ctype.h>
#include <time.h>

static ObjFile *cg_obj;
static SecBuf *cg_sec;
bool cg_dry_run; // pass 1: track regs only (extern for codegen_asm.h)
uint64_t time_peep_us = 0;

static void cg_set_section(int sec) {
    if (!cg_obj) return;
    switch (sec) {
    case SEC_TEXT: cg_sec = &cg_obj->text; break;
    case SEC_DATA: cg_sec = &cg_obj->data; break;
    case SEC_RODATA: cg_sec = &cg_obj->rodata; break;
    default: cg_sec = &cg_obj->text; break;
    }
}

static void cg_def_label(const char *name) {
    if (cg_dry_run) return;
    objfile_add_sym(cg_obj, name, SEC_TEXT, cg_sec->len, 0, SB_LOCAL, ST_FUNC);
    cg_label_ht_add(name, cg_sec->len);
    asm_fixup_resolve(cg_sec, name, cg_sec->len);
}

static void cg_def_label_sec(const char *name, int sec) {
    if (cg_dry_run) return;
    objfile_add_sym(cg_obj, name, sec, cg_sec->len, 0, SB_LOCAL, ST_OBJECT);
    cg_label_ht_add(name, cg_sec->len);
    asm_fixup_resolve(cg_sec, name, cg_sec->len);
}

static void cg_global_label(const char *name) {
    if (cg_dry_run) return;
    objfile_add_sym(cg_obj, name, SEC_TEXT, cg_sec->len, 0, SB_GLOBAL, ST_FUNC);
    cg_label_ht_add(name, cg_sec->len);
    asm_fixup_resolve(cg_sec, name, cg_sec->len);
}

static void cg_weak_label(const char *name) {
    if (cg_dry_run) return;
    objfile_add_sym(cg_obj, name, SEC_TEXT, cg_sec->len, 0, SB_WEAK, ST_FUNC);
}

#ifndef ARCH_ARM64
static size_t asm_lea_rip_reg(SecBuf *s, int r, const char *label) {
    size_t off = s->len;
    X86Mem m = {X86_RIP, X86_NOREG, 1, 0};
    x86_lea(s, 8, CG_X86_REG(r), m);
    if (!cg_dry_run) {
        int sidx = objfile_find_sym(cg_obj, label);
        if (sidx < 0) {
            bool is_local_label = label[0] == '.';
            sidx = objfile_add_sym(cg_obj, label, SEC_UNDEF, 0, 0, is_local_label ? SB_LOCAL : SB_GLOBAL, ST_NOTYPE);
        }
        objfile_add_reloc(cg_obj, SEC_TEXT, off + 3, sidx, R_X86_64_PC32, -4);
    }
    asm_record(ASM_LEA_FP, off, s->len - off, r, -1, -1, 8, 0, 0, label, 0, -1, false);
    return s->len - off;
}
#endif

static uint64_t cg_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}
static Function *current_fn_def;
static TLItem *all_items;
static StrLit *all_strs;

static char *current_fn;
static int current_fn_stack_size = 0; // fn->stack_size of the function being generated

// Debug info (DWARF .loc directives, enabled by -g)
#define MAX_DEBUG_FILES 128
static char *debug_files[MAX_DEBUG_FILES];
static int debug_file_count = 0;
static int last_debug_file = 0;
static int last_debug_line = 0;

static int get_debug_file_idx(char *filename) {
    if (!filename) return 1;
    for (int i = 0; i < debug_file_count; i++)
        if (strcmp(debug_files[i], filename) == 0)
            return i + 1;
    if (debug_file_count >= MAX_DEBUG_FILES)
        return 1;
    int idx = ++debug_file_count;
    debug_files[idx - 1] = filename;
    (void)0 /* .file {idx} \"{filename}\" */;
    return idx;
}

static void emit_loc(Node *node) {
    if (!node->tok || !node->tok->filename)
        return;
    int line = node->tok->lineno;
    if (line <= 0) return;
    int fidx = get_debug_file_idx(node->tok->filename);
    if (fidx == last_debug_file && line == last_debug_line)
        return;
    (void)0 /* .loc {fidx} {line} 0 */;
    last_debug_file = fidx;
    last_debug_line = line;
}
static int fn_struct_ret_off = 0; // next free offset within the struct-ret-buf area
static int fn_struct_ret_total = 0; // high-water mark of struct-ret-buf space used
static int rcc_label_count = 0;
static int va_gp_start;
static int va_fp_start;
static int va_st_start;
#ifndef ARCH_ARM64
static int va_reg_save_ofs;
#endif
static int break_stack[128];
static int continue_stack[128];
static int ctrl_depth = 0;
static int float_lit_count = 0;

typedef struct FloatLit FloatLit;
struct FloatLit {
    FloatLit *next;
    int id;
    double val;
    int size; // 4=float, 8=double, TODO 12|16=long double
};
static FloatLit *float_lits;
static bool alloca_needed;
static bool fn_uses_alloca;

#ifdef ARCH_ARM64
// Arm64: 12 allocatable GP registers
// x0-x7: args/return, x8: indirect result, x9-x15: caller-saved,
// x16-x18: reserved, x19-x28: callee-saved, x29: fp, x30: lr, sp/xzr
// We use x10-x15 (6 caller-saved) and x19-x24 (6 callee-saved) = 12 allocatable
static char *reg64[] = {"x10", "x11", "x12", "x13", "x14", "x15", "x19", "x20", "x21", "x22", "x23", "x24"};
static char *reg32[] = {"w10", "w11", "w12", "w13", "w14", "w15", "w19", "w20", "w21", "w22", "w23", "w24"};
// Arm64 has no dedicated 8/16-bit registers; use w regs, mask after load/store
#define reg16 reg32
#define reg8  reg32
#define NUM_REGS 12
#define FRAME_PTR "x29"
#define LINK_REG  "x30"
#define STACK_REG "sp"

#else
// x86_64: 8 allocatable GP registers
static char *reg64[] = {"%r10", "%r11", "%rbx", "%r12", "%r13", "%r14", "%r15", "%rsi"};
static char *reg32[] = {"%r10d", "%r11d", "%ebx", "%r12d", "%r13d", "%r14d", "%r15d", "%esi"};
static char *reg16[] = {"%r10w", "%r11w", "%bx", "%r12w", "%r13w", "%r14w", "%r15w", "%si"};
static char *reg8[] = {"%r10b", "%r11b", "%bl", "%r12b", "%r13b", "%r14b", "%r15b", "%sil"};
#define NUM_REGS 8
#define FRAME_PTR "rbp"
#define STACK_REG "rsp"
#endif

static int used_regs = 0;
static int ever_used_regs = 0;

#ifdef ARCH_ARM64
static void emit_mov_imm64(const char *reg, uint64_t val);

// Arm64: spill to [x29, #-N]; offsets grown from frame base
static int spill_slot[NUM_REGS];
static int next_spill_slot;

static int spill_offset(int r) {
    if (!spill_slot[r]) {
        next_spill_slot += 8;
        spill_slot[r] = next_spill_slot;
    }
    return spill_slot[r];
}
#else
// Spill slot offsets from rbp for register spilling (dynamic, grows from 8)
static int spill_slot[NUM_REGS];
static int next_spill_slot;
static int spill_logand, spill_atomic_old;
#define ALL_REGS_MASK ((1 << NUM_REGS) - 1)

static int alloc_spill_slot(void);

static void init_spill_slots(void) {
    memset(spill_slot, 0, sizeof(spill_slot));
    next_spill_slot = 8;
    spill_logand = alloc_spill_slot();
    spill_atomic_old = alloc_spill_slot();
}

static int alloc_spill_slot(void) {
    int slot = next_spill_slot;
    next_spill_slot += 8;
    return slot;
}

static int spill_offset(int r) {
    if (!spill_slot[r]) {
        spill_slot[r] = alloc_spill_slot();
    }
    return spill_slot[r];
}
#endif

static int spilled_regs = 0;
static int spill_count = 0;
static const char *reg_owner[NUM_REGS];

static char *reg(int r, int size);
static int alloc_reg(void);
static void free_reg(int i);
static int gen(Node *node);
static int gen_addr(Node *node);
static bool is_asm_reserved(const char *name);
static void sign_extend_to(int r, int from_size, int to_size);
static void zero_extend_to(int r, int from_size, int to_size);

static bool var_needs_got(LVar *var) {
    if (var->is_local) return false;
    if (var->is_static) return false;
    return opt_pic;
}

#if 0
static char *func_asm_name(char *name) {
    for (TLItem *item = all_items; item; item = item->next) {
        if (item->kind == TL_FUNC && item->fn->name == name)
            return item->fn->asm_name ? item->fn->asm_name : item->fn->name;
    }
    return name;
}
#endif

// Mach-O symbols get a leading underscore
static const char *sym_name(const char *name) {
#if defined(__APPLE__)
    if (name[0] == '.' || name[0] == '/')
        return name;
    return format("_%s", name);
#else
    return name;
#endif
}

// Assembly label for a variable: respects __asm__ names (used as-is) and
// applies sym_name() to regular C identifiers.
static const char *var_sym_label(LVar *var) {
    if (var->asm_name) return var->asm_name;
    if (is_asm_reserved(var->name)) return format(".L_rcc_%s", var->name);
    return sym_name(var->name);
}

// Assembly label for a function: respects __asm__ names (used as-is) and
// applies sym_name() to regular C identifiers.
static const char *func_label(char *name) {
    for (TLItem *item = all_items; item; item = item->next)
        if (item->kind == TL_FUNC && !strcmp(item->fn->name, name)) {
            if (item->fn->asm_name)
                return item->fn->asm_name;
            return sym_name(item->fn->name);
        }
    return sym_name(name);
}

// Assembly-safe symbol name: quotes non-ASCII identifiers for LLVM assembler
static const char *asm_sym_name(const char *name) {
#if defined(__APPLE__)
    const char *s = name;
    while (*s) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E)
            return format("\"%s\"", name);
        s++;
    }
#endif
    return name;
}

static void emit_direct_call(char *name) {
    if (cg_dry_run) return;
    if (is_asm_reserved(name))
        name = format(".L_rcc_%s", name);
    const char *label = func_label(name);
    size_t off = asm_call_label(cg_sec); // bl %s
    int sidx = objfile_find_sym(cg_obj, label);
    if (sidx < 0)
        sidx = objfile_add_sym(cg_obj, label, SEC_UNDEF, 0, 0, SB_GLOBAL, ST_FUNC);
#ifdef ARCH_ARM64
    // bl label
    objfile_add_reloc(cg_obj, SEC_TEXT, off, sidx, R_AARCH64_CALL26, 0);
#else
    // call label
    objfile_add_reloc(cg_obj, SEC_TEXT, off + 1, sidx, R_X86_64_PLT32, -4);
#endif
}

static bool var_has_cleanup(LVar *var) {
    if (!var->is_local) return false;
    if (var->cleanup_func) return true;
    return var->ty->kind == TY_ARRAY && var->ty->base && var->ty->base->cleanup_func;
}

static void emit_cleanup_var(LVar *var) {
    if (var->cleanup_func) {
#ifdef ARCH_ARM64
        if (var->offset <= 4095)
            asm_add_x0_fp_imm(cg_sec, -var->offset); // add x0, x29, #{-var->offset}
        else {
            int v = var->offset;
            asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // mov x16, #v
            v >>= 16;
            int s = 16;
            while (v) {
                asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // movk x16, #v, lsl #s
                v >>= 16;
                s += 16;
            }
            asm_sub_x0_fp_x16(cg_sec); // sub x0, x29, x16
        }
#elif defined(_WIN32)
        asm_lea_rbp_phy(cg_sec, X86_RCX, 8, var->offset); // leaq -%d(%%rbp), %%rcx
#else
        asm_lea_rbp_phy(cg_sec, X86_RDI, 8, var->offset); // leaq -%d(%%rbp), %%rdi
#endif
        emit_direct_call(var->cleanup_func);
        return;
    }
    // Array whose element type carries __cleanup__: call per element, LIFO
    char *func = var->ty->base->cleanup_func;
    int elem_size = var->ty->base->size;
    int nelem = elem_size ? var->ty->size / elem_size : 0;
    for (int i = nelem - 1; i >= 0; i--) {
#ifdef ARCH_ARM64
        int off = var->offset - i * elem_size;
        if (off <= 4095)
            asm_add_x0_fp_imm(cg_sec, -off);
        else {
            int v = off;
            asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // mov x16, #%d
            v >>= 16;
            int s = 16;
            while (v) {
                asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // movk x16, #%d, lsl #%d
                v >>= 16;
                s += 16;
            }
            asm_sub_x0_fp_x16(cg_sec);
        }
#elif defined(_WIN32)
        asm_lea_rbp_phy(cg_sec, X86_RCX, 8, var->offset - i * elem_size); // leaq -%d(%%rbp), %%rcx
#else
        asm_lea_rbp_phy(cg_sec, X86_RDI, 8, var->offset - i * elem_size); // leaq -%d(%%rbp), %%rdi
#endif
        emit_direct_call(func);
    }
}

static void emit_cleanup_range(LVar *begin, LVar *end) {
    for (LVar *var = begin; var && var != end; var = var->next) {
        if (var_has_cleanup(var))
            emit_cleanup_var(var);
    }
}

#ifdef ARCH_ARM64
static int arm64_hfa_info(Type *ty, int *elem_size) {
    if (!ty)
        return -1;
    if (is_flonum(ty) && (ty->size == 4 || ty->size == 8)) {
        if (*elem_size == 0)
            *elem_size = ty->size;
        else if (*elem_size != ty->size)
            return -1;
        return 1;
    }
    if (ty->kind != TY_STRUCT)
        return -1;
    int count = 0;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        int sub = arm64_hfa_info(mem->ty, elem_size);
        if (sub <= 0)
            return -1;
        count += sub;
        if (count > 4)
            return -1;
    }
    return count > 0 ? count : -1;
}

static int arm64_hfa_count(Type *ty, int *elem_size) {
    int es = 0;
    int n = arm64_hfa_info(ty, &es);
    if (n <= 0) {
        if (elem_size)
            *elem_size = 0;
        return 0;
    }
    if (elem_size)
        *elem_size = es;
    return n;
}
#endif

#ifdef ARCH_ARM64
// ARM64: emit load from [fp, #-offset] into x16 (uses x17 for large offsets)
static void arm64_load_from_fp_minus(int offset, const char *dst) {
    (void)dst;
    if (offset <= 255)
        asm_ldur_x16_fp_minus(cg_sec, offset); // ldr %s, [%s, #-%d]
    else if (offset <= 4095) {
        asm_sub_x17_fp_imm(cg_sec, offset); // sub x17, %s, #%d
        asm_ldr_x16_x17(cg_sec); // ldr %s, [x17]
    } else {
        int v = offset;
        asm_mov_imm(cg_sec, 17, 8, v & 0xffff); // mov x17, #%d
        v >>= 16;
        int s = 16;
        while (v) {
            asm_movk(cg_sec, 17, 1, (uint16_t)(v & 0xffff), s); // movk x17, #%d, lsl #%d
            v >>= 16;
            s += 16;
        }
        asm_sub_x17_fp_x17(cg_sec); // sub x17, %s, x17
        asm_ldr_x16_x17(cg_sec); // ldr %s, [x17]
    }
}

// ARM64: emit store src to [fp, #-offset] (uses x17 for large offsets)
static void arm64_store_to_fp_minus(const char *src, int offset) {
    (void)src;
    if (offset <= 255)
        asm_stur_x16_fp_minus(cg_sec, offset); // str %s, [%s, #-%d]
    else if (offset <= 4095) {
        asm_sub_x17_fp_imm(cg_sec, offset); // sub x17, %s, #%d
        asm_str_x16_x17(cg_sec); // str %s, [x17]
    } else {
        int v = offset;
        asm_mov_imm(cg_sec, 17, 8, v & 0xffff); // mov x17, #%d
        v >>= 16;
        int s = 16;
        while (v) {
            asm_movk(cg_sec, 17, 1, (uint16_t)(v & 0xffff), s); // movk x17, #%d, lsl #%d
            v >>= 16;
            s += 16;
        }
        asm_sub_x17_fp_x17(cg_sec); // sub x17, %s, x17
        asm_str_x16_x17(cg_sec); // str %s, [x17]
    }
}
#endif

// Restore SP past any VLAs leaving scope (enables VLA reuse on re-entry).
// Locals list is newest-first; the last VLA in range is the outermost one.
static void emit_vla_dealloc(LVar *begin, LVar *end) {
    LVar *outermost_vla = NULL;
    for (LVar *v = begin; v && v != end; v = v->next)
        if (v->ty->kind == TY_VLA || ((v->ty->kind == TY_STRUCT || v->ty->kind == TY_UNION) && v->ty->vla_len_expr))
            outermost_vla = v;
    if (outermost_vla) {
#ifdef ARCH_ARM64
        arm64_load_from_fp_minus(outermost_vla->offset, "x16");
        asm_vla_mov_sp_x16(cg_sec); // mov sp, x16
#else
        asm_mov_rbp_phyreg(cg_sec, X86_RSP, 8, outermost_vla->offset); // movq -%d(%%rbp), %%rsp
#endif
    }
}

/* Other platforms still have it. windows deprecated it.
   Use a unique name to avoid conflicts with CRT import stubs. */
static void emit_alloca(void) {
    cg_global_label("__rcc_alloca");
#ifdef ARCH_ARM64
    asm_alloca_add(cg_sec); // \n%s:\n  popq %%rdx\n", sym_name("__rcc_alloca
    asm_alloca_and(cg_sec); // movq %%rcx, %%rax
    asm_alloca_sub_sp_r0(cg_sec); // movq %%rdi, %%rax
    asm_alloca_mov_r0_sp(cg_sec); // addq $15, %%rax\n  andq $-16, %%rax\n  jz .Lalloca3
    asm_ret(cg_sec); // .Lalloca1:\n  cmpq $4096, %%rax\n  jb .Lalloca2\n  testq %%rax, -4096(%%rsp)\n  subq $4096, %%rsp\n  subq $4096, %%rax\n  jmp .Lalloca1
#else
    x86_mov_rr(cg_sec, 8, X86_RAX, X86_RDI); // .Lalloca2:\n  subq %%rax, %%rsp\n  movq %%rsp, %%rax\n.Lalloca3:\n  pushq %%rdx\n  ret
    x86_add_ri(cg_sec, 8, X86_RAX, 15); // \n%s:\n  popq %%rdx\n", sym_name("__rcc_alloca
    x86_and_ri(cg_sec, 8, X86_RAX, -16); // movq %%rcx, %%rax
    x86_sub_rr(cg_sec, 8, X86_RSP, X86_RAX); // movq %%rdi, %%rax
    x86_mov_rr(cg_sec, 8, X86_RAX, X86_RSP); // addq $15, %%rax\n  andq $-16, %%rax\n  jz .Lalloca3
    x86_ret(cg_sec); // .Lalloca1:\n  cmpq $4096, %%rax\n  jb .Lalloca2\n  testq %%rax, -4096(%%rsp)\n  subq $4096, %%rsp\n  subq $4096, %%rax\n  jmp .Lalloca1
#endif
}

static char *var_label(LVar *var);


bool va_arg_need_copy(Type *ty) {
    if (ty->size > 8 && ty->size <= 16) {
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            for (Member *m = ty->members; m; m = m->next) {
                if (is_flonum(m->ty))
                    return true;
            }
        }
    }
    return false;
}

static int gen_funcall(Node *node, int hidden_ret_reg) {
    int nargs = 0;
    for (Node *arg = node->args; arg; arg = arg->next)
        nargs++;

    Node *argv_stk[64];
    int arg_regs_stk[64];
    int arg_sizes_stk[64];
    bool arg_is_float_stk[64];
#ifndef _WIN32
    int arg_gp_idx_stk[64];
    int arg_fp_idx_stk[64];
    int arg_stack_idx_stk[64];
#ifdef ARCH_ARM64
    int arg_hfa_count_stk[64];
    int arg_hfa_elem_size_stk[64];
#endif
#endif

    Node **argv;
    int *arg_regs;
    int *arg_sizes;
    bool *arg_is_float;
#ifndef _WIN32
    int *arg_gp_idx;
    int *arg_fp_idx;
    int *arg_stack_idx;
#ifdef ARCH_ARM64
    int *arg_hfa_count;
    int *arg_hfa_elem_size;
#endif
#endif

    if (nargs <= 64) {
        argv = argv_stk;
        arg_regs = arg_regs_stk;
        arg_sizes = arg_sizes_stk;
        arg_is_float = arg_is_float_stk;
#ifndef _WIN32
        arg_gp_idx = arg_gp_idx_stk;
        arg_fp_idx = arg_fp_idx_stk;
        arg_stack_idx = arg_stack_idx_stk;
#ifdef ARCH_ARM64
        arg_hfa_count = arg_hfa_count_stk;
        arg_hfa_elem_size = arg_hfa_elem_size_stk;
#endif
#endif
    } else {
        argv = arena_alloc(sizeof(Node *) * nargs);
        arg_regs = arena_alloc(sizeof(int) * nargs);
        arg_sizes = arena_alloc(sizeof(int) * nargs);
        arg_is_float = arena_alloc(sizeof(bool) * nargs);
#ifndef _WIN32
        arg_gp_idx = arena_alloc(sizeof(int) * nargs);
        arg_fp_idx = arena_alloc(sizeof(int) * nargs);
        arg_stack_idx = arena_alloc(sizeof(int) * nargs);
#ifdef ARCH_ARM64
        arg_hfa_count = arena_alloc(sizeof(int) * nargs);
        arg_hfa_elem_size = arena_alloc(sizeof(int) * nargs);
#endif
#endif
    }

    int idx = 0;
    for (Node *arg = node->args; arg; arg = arg->next)
        argv[idx++] = arg;

    char *call_target = node->funcname;
    if (call_target && is_asm_reserved(call_target))
        call_target = format(".L_rcc_%s", call_target);
    if (!call_target && node->lhs && node->lhs->kind == ND_LVAR &&
        node->lhs->var && node->lhs->var->is_function)
        call_target = var_label(node->lhs->var);

#ifdef _WIN32
    char *argreg32[] = {"%ecx", "%edx", "%r8d", "%r9d"};
    char *argreg64[] = {"%rcx", "%rdx", "%r8", "%r9"};
    char *argxmm[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};
    int shadow_space = 32;
    int max_gp_args = 4;
#elif defined(ARCH_ARM64)
    // AAPCS64: 8 GP arg regs (x0-x7), 8 SIMD/FP arg regs (v0-v7)
    // x8 is the indirect result register for struct returns
    char *argreg32[] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
    char *argreg64[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    char *argxmm[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    int max_gp_args = 8;
    int max_fp_args = 8;
    //int shadow_space = 0;
#else
    char *argreg32[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
    char *argreg64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
    X86Reg cg_x86_argreg[] = {X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9};
    char *argxmm[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7"};
    int max_gp_args = 6;
    int max_fp_args = 8;
    int shadow_space = 0;
#endif

    bool has_hidden_retbuf = node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION);

    // Cross-architecture builtins (x86_64 and arm64)
    if (call_target && !has_hidden_retbuf) {
        bool is_bswap16 = strcmp(call_target, "__builtin_bswap16") == 0;
        bool is_bswap32 = strcmp(call_target, "__builtin_bswap32") == 0;
        bool is_bswap64 = strcmp(call_target, "__builtin_bswap64") == 0;
        bool is_clz = strcmp(call_target, "__builtin_clz") == 0;
        bool is_clzl = strcmp(call_target, "__builtin_clzl") == 0;
        bool is_clzll = strcmp(call_target, "__builtin_clzll") == 0;
        bool is_ctz = strcmp(call_target, "__builtin_ctz") == 0;
        bool is_ctzl = strcmp(call_target, "__builtin_ctzl") == 0;
        bool is_ctzll = strcmp(call_target, "__builtin_ctzll") == 0;
        bool is_popcnt = strcmp(call_target, "__builtin_popcount") == 0;
        bool is_popcntl = strcmp(call_target, "__builtin_popcountl") == 0;
        bool is_popcntll = strcmp(call_target, "__builtin_popcountll") == 0;
        bool is_parity = strcmp(call_target, "__builtin_parity") == 0;
        bool is_parityl = strcmp(call_target, "__builtin_parityl") == 0;
        bool is_parityll = strcmp(call_target, "__builtin_parityll") == 0;
        bool is_clrsb = strcmp(call_target, "__builtin_clrsb") == 0;
        bool is_clrsbl = strcmp(call_target, "__builtin_clrsbl") == 0;
        bool is_clrsbll = strcmp(call_target, "__builtin_clrsbll") == 0;
        bool is_ffs = strcmp(call_target, "__builtin_ffs") == 0;
        bool is_ffsl = strcmp(call_target, "__builtin_ffsl") == 0;
        bool is_ffsll = strcmp(call_target, "__builtin_ffsll") == 0;
        bool is_prefetch = strcmp(call_target, "__builtin_prefetch") == 0;
        bool is_frame_addr = strcmp(call_target, "__builtin_frame_address") == 0;
        bool is_ret_addr = strcmp(call_target, "__builtin_return_address") == 0;
#ifdef ARCH_ARM64
        bool is_setjmp = strcmp(call_target, "__builtin_setjmp") == 0;
        bool is_longjmp = strcmp(call_target, "__builtin_longjmp") == 0;
#endif
        bool is_signbit = strcmp(call_target, "__builtin_signbit") == 0 ||
            strcmp(call_target, "__builtin_signbitf") == 0 ||
            strcmp(call_target, "__builtin_signbitl") == 0;
        bool is_abs_builtin = strcmp(call_target, "__builtin_abs") == 0 ||
            strcmp(call_target, "__builtin_labs") == 0 ||
            strcmp(call_target, "__builtin_llabs") == 0 ||
            strcmp(call_target, "abs") == 0 ||
            strcmp(call_target, "labs") == 0 ||
            strcmp(call_target, "llabs") == 0;
        bool is_add_overflow = strcmp(call_target, "__builtin_add_overflow") == 0;
        bool is_sub_overflow = strcmp(call_target, "__builtin_sub_overflow") == 0;
        bool is_mul_overflow = strcmp(call_target, "__builtin_mul_overflow") == 0;
        bool is_mul_overflow_p = strcmp(call_target, "__builtin_mul_overflow_p") == 0;

        if (is_bswap16 || is_bswap32 || is_bswap64) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
#ifdef ARCH_ARM64
                if (is_bswap16) {
                    asm_rev16(cg_sec, r, 4); // rev16 %s, %s
                    (void)0 /* FIXME: and variant */;
                } else if (is_bswap32) {
                    asm_rev(cg_sec, r, 4); // and %s, %s, #0xffff
                } else {
                    asm_rev(cg_sec, r, 8); // rev %s, %s
                }
#else
                if (is_bswap16) {
                    (void)0 /* FIXME: rol */;
                    asm_movzx(cg_sec, r, r, 4, 2); // rev %s, %s
                } else if (is_bswap32) {
                    asm_bswap(cg_sec, r, 4); // rol $8, %s
                } else {
                    asm_bswap(cg_sec, r, 8); // movzwl %s, %s
                }
#endif
                return r;
            }
        }

        if (is_clz || is_clzl || is_clzll) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
                int r2 = alloc_reg();
                bool is64 = is_clzll || (is_clzl && sizeof(long) == 8);
#ifdef ARCH_ARM64
                if (is64)
                    asm_clz(cg_sec, r2, r, 8); // clz %s, %s
                else
                    asm_clz(cg_sec, r2, r, 4); // clz %s, %s
#else
                if (is64) {
                    (void)0 /* FIXME: bit scan */;
                } else {
                    (void)0 /* FIXME: bit scan */;
                }
#endif
                free_reg(r);
                return r2;
            }
        }

        if (is_ctz || is_ctzl || is_ctzll) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
                int r2 = alloc_reg();
                bool is64 = is_ctzll || (is_ctzl && sizeof(long) == 8);
#ifdef ARCH_ARM64
                if (is64) {
                    asm_rbit(cg_sec, r, r, 8); // rbit %s, %s
                    asm_clz(cg_sec, r2, r, 8); // clz %s, %s
                } else {
                    asm_rbit(cg_sec, r, r, 4); // rbit %s, %s
                    asm_clz(cg_sec, r2, r, 4); // clz %s, %s
                }
#else
                if (is64) {
                    (void)0 /* FIXME: bit scan */;
                } else {
                    (void)0 /* FIXME: bit scan */;
                }
#endif
                free_reg(r);
                return r2;
            }
        }

        if (is_popcnt || is_popcntl || is_popcntll || is_parity || is_parityl || is_parityll) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
                int r2 = alloc_reg();
                bool is64 = is_popcntll || is_parityll ||
                    ((is_popcntl || is_parityl) && sizeof(long) == 8);
#ifdef ARCH_ARM64
                // Software popcount via NEON cnt
                char *tmp = is64 ? "v30.8b" : "v30.8b";
                if (is64) {
                    (void)0 /* FIXME: fmov */;
                } else {
                    (void)0 /* FIXME: fmov */;
                }
                (void)0 /* FIXME: NEON */;
                (void)0 /* FIXME: NEON */;
                (void)0 /* FIXME: fmov */;
                (void)0 /* FIXME: and variant */;
                (void)tmp;
#else
                if (is64) {
                    (void)0 /* FIXME: bit scan */;
                } else {
                    (void)0 /* FIXME: bit scan */;
                }
#endif
                if (is_parity || is_parityl || is_parityll)
#ifdef ARCH_ARM64
                    (void)0 /* FIXME: and variant */;
#else
                    (void)0 /* FIXME: and variant */;
#endif
                free_reg(r);
                return r2;
            }
        }

        if (is_clrsb || is_clrsbl || is_clrsbll) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
                int r2 = alloc_reg();
                bool is64 = is_clrsbll || (is_clrsbl && sizeof(long) == 8);
#ifdef ARCH_ARM64
                if (is64) {
                    (void)0 /* FIXME: bit scan */;
                } else {
                    (void)0 /* FIXME: bit scan */;
                }
#else
                // clrsb(x) = (x>=0 ? clz(x) : clz(~x)) - 1
                int lbl = ++rcc_label_count;
                int r3 = alloc_reg();
                if (is64) {
                    asm_mov_reg_reg(cg_sec, r, r3, 8); // cls %s, %s
                    (void)0 /* FIXME: shift imm */;
                    asm_xor_reg_reg(cg_sec, r3, r, 8); // cls %s, %s
                    (void)0 /* FIXME: bit scan */;
                    asm_dec(cg_sec, r2, 8); // mov %s, %s
                } else {
                    asm_mov_reg_reg(cg_sec, r, r3, 4); // sar $63, %s
                    (void)0 /* FIXME: shift imm */;
                    asm_xor_reg_reg(cg_sec, r3, r, 4); // xor %s, %s
                    (void)0 /* FIXME: bit scan */;
                    asm_dec(cg_sec, r2, 4); // lzcnt %s, %s
                }
                free_reg(r3);
                (void)lbl;
#endif
                free_reg(r);
                return r2;
            }
        }

        if (is_ffs || is_ffsl || is_ffsll) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
                int r2 = alloc_reg();
                int r3 = alloc_reg();
                bool is64 = is_ffsll || (is_ffsl && sizeof(long) == 8);
#ifdef ARCH_ARM64
                if (is64) {
                    asm_rbit(cg_sec, r, r, 8); // rbit %s, %s
                    asm_clz(cg_sec, r3, r, 8); // clz %s, %s
                    asm_add_imm(cg_sec, r3, 8, 1); // add %s, %s, #1
                    asm_cmp_zero(cg_sec, r, 8); // cmp %s, #0
                    (void)0 /* FIXME: cmov/csel/cneg */;
                } else {
                    asm_rbit(cg_sec, r, r, 4); // csel %s, xzr, %s, eq
                    asm_clz(cg_sec, r3, r, 4); // rbit %s, %s
                    asm_add_imm(cg_sec, r3, 4, 1); // clz %s, %s
                    asm_cmp_zero(cg_sec, r, 4); // add %s, %s, #1
                    (void)0 /* FIXME: cmov/csel/cneg */;
                }
#else
                if (is64) {
                    asm_movq_zero(cg_sec, r2); // cmp %s, #0
                    (void)0 /* FIXME: bit scan */;
                    (void)0 /* FIXME: lea */;
                    (void)0 /* FIXME: cmov/csel/cneg */;
                } else {
                    (void)0 /* FIXME: movl imm */;
                    (void)0 /* FIXME: bit scan */;
                    (void)0 /* FIXME: lea */;
                    (void)0 /* FIXME: cmov/csel/cneg */;
                }
#endif
                free_reg(r3);
                free_reg(r);
                return r2;
            }
        }

        if (is_prefetch) {
            Node *addr = node->args;
            int rw = 0, locality = 3;
            // Parse rw and locality from constant args
            if (addr && addr->next && addr->next->kind == ND_NUM)
                rw = (int)addr->next->val;
            if (addr && addr->next && addr->next->next && addr->next->next->kind == ND_NUM)
                locality = (int)addr->next->next->val;
            if (addr) {
                int r = gen(addr);
#ifdef ARCH_ARM64
                // prfm: pld/pst + l1/l2/l3 + keep/strm
                const char *hint = "pldl1keep";
                if (rw == 1 && locality == 0) hint = "pstl1strm";
                else if (rw == 1)
                    hint = "pstl1keep";
                else if (locality == 0)
                    hint = "pldl1strm";
                (void)0 /* FIXME: prfm */;
#else
                // x86: prefetchw for write, otherwise nta/t0/t1/t2 by locality
                const char *hint = (rw == 1) ? "prefetchw" : locality == 0 ? "prefetchnta"
                    : locality == 1                                        ? "prefetcht2"
                    : locality == 2                                        ? "prefetcht1"
                                                                           : "prefetcht0";
                (void)0 /* FIXME: prefix (indirect) */;
#endif
                free_reg(r);
                // Evaluate remaining args for side effects (if any expressions)
                for (Node *a = addr->next; a; a = a->next) {
                    if (a->kind != ND_NUM) {
                        int ar = gen(a);
                        if (ar >= 0) free_reg(ar);
                    }
                }
            }
            return -1; // void
        }

        if (is_signbit) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r_arg = gen(arg);
                int r = alloc_reg();
#ifdef ARCH_ARM64
                asm_shr_imm(cg_sec, r, 8, 63); // lsr %s, %s, #63
#else
                (void)0 /* TODO: movq to/from xmm */;
                (void)0 /* TODO: movq to/from xmm */;
                (void)0 /* FIXME: shift imm */;
#endif
                free_reg(r_arg);
                return r;
            }
        }

        if (is_abs_builtin) {
            Node *arg = node->args;
            if (arg && !arg->next) {
                int r = gen(arg);
#ifdef ARCH_ARM64
                int arg_size = (arg->ty && !is_flonum(arg->ty)) ? arg->ty->size : 8;
                if (arg_size <= 4 && !(arg->ty && arg->ty->is_unsigned))
                    asm_movsx(cg_sec, r, r, 8, 4); // sxtw %s, %s
                asm_cmp_zero(cg_sec, r, 8); // cmp %s, #0
                (void)0 /* FIXME: cmov/csel/cneg */;
                return r;
#else
                int r2 = alloc_reg();
                int arg_size = (arg->ty && !is_flonum(arg->ty)) ? arg->ty->size : 8;
                if (arg_size <= 4 && !(arg->ty && arg->ty->is_unsigned))
                    asm_movsx(cg_sec, r, r, 8, 4); // cneg %s, %s, mi
                asm_mov_reg_reg(cg_sec, r, r2, 8); // movslq %s, %s
                (void)0 /* FIXME: shift imm */;
                (void)0 /* FIXME: sized alu op */;
                (void)0 /* FIXME: sized alu op */;
                free_reg(r2);
                return r;
#endif
            }
        }

        if (is_frame_addr) {
            Node *arg = node->args;
            int r = alloc_reg();
            int depth = (arg && arg->kind == ND_NUM) ? (int)arg->val : 0;
#ifdef ARCH_ARM64
            asm_mov_reg_reg(cg_sec, r, 5, 8); // mov %s, " FRAME_PTR "
            for (int i = 0; i < depth; i++)
                asm_ldr_reg_off(cg_sec, r, r, 8, 0); // ldr %s, [%s]
#else
            asm_mov_reg_reg(cg_sec, r, 5, 8); // movq %%rbp, %s
            for (int i = 0; i < depth; i++)
                (void)0 /* FIXME: indirect mov */;
#endif
            return r;
        }
        if (is_ret_addr) {
            Node *arg = node->args;
            int r = alloc_reg();
            int depth = (arg && arg->kind == ND_NUM) ? (int)arg->val : 0;
            // Follow frame pointer chain to find the return address.
            // frame pointer → [fp] = saved fp, [fp+8] = return address
#ifdef ARCH_ARM64
            asm_mov_reg_reg(cg_sec, r, 5, 8); // mov %s, " FRAME_PTR "
            for (int i = 0; i < depth; i++)
                asm_ldr_reg_off(cg_sec, r, r, 8, 0); // ldr %s, [%s]
            (void)0 /* FIXME: ldr/str phy/off */;
#else
            asm_mov_reg_reg(cg_sec, r, 5, 8); // ldr %s, [%s, #8]
            for (int i = 0; i < depth; i++)
                (void)0 /* FIXME: indirect mov */;
            (void)0 /* FIXME: mov indirect/mem */;
#endif
            return r;
        }
#ifdef ARCH_ARM64
        if (is_setjmp) {
            // Inline __builtin_setjmp: save fp, resume_addr, sp to buf; return 0 or 1 (longjmp)
            int rbuf = gen(node->args);
            int c = ++rcc_label_count;
            int r = alloc_reg();
            asm_str_fp_reg(cg_sec, rbuf); // movq %%rbp, %s
            (void)0 /* FIXME: adrp/adr */;
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: mov phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: mov phy */;
            asm_jmp_label(cg_sec); // mov (%s), %s
            (void)0 /* FIXME: label .L.xxx.c */;
            cg_def_label(format(".L.setjmp.%d", c)); // mov 8(%s), %s
            asm_mov_reg_reg(cg_sec, r, 0, 8); // str %s, [%s]
            free_reg(rbuf);
            return r;
        }
        if (is_longjmp) {
            // Inline __builtin_longjmp: restore fp, sp from buf; jump to resume addr with val
            int rbuf = gen(node->args);
            int rval = gen(node->args->next);
            asm_ldr_fp_reg(cg_sec, rbuf); // ldr %s, [%s]
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: mov phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: mov phy */;
            asm_jmp_reg(cg_sec, 16); // ldr x16, [%s, #16]
            free_reg(rbuf);
            free_reg(rval);
            return -1;
        }
        if (is_add_overflow || is_sub_overflow || is_mul_overflow || is_mul_overflow_p) {
            Node *arga = node->args;
            Node *argb = arga ? arga->next : NULL;
            Node *argres = argb ? argb->next : NULL;
            int ra = gen(arga);
            int rb = gen(argb);
            int sz = arga && arga->ty && arga->ty->size > 4 ? 8 : 4;
            bool is_unsigned = arga && arga->ty && arga->ty->is_unsigned;
            int r_result = alloc_reg();
            if (is_add_overflow) {
                (void)0 /* FIXME: adds */;
                // cs = unsigned carry, vs = signed overflow
                (void)0 /* FIXME: cset with var cond */;
            } else if (is_sub_overflow) {
                (void)0 /* FIXME: subs */;
                (void)0 /* FIXME: cset with var cond */;
            } else { // mul_overflow / mul_overflow_p
                int r2 = alloc_reg();
                if (sz == 8) {
                    if (is_unsigned) {
                        (void)0 /* FIXME: NEON */;
                        asm_mul_reg_reg(cg_sec, ra, ra, 8); // adds %s, %s, %s
                        asm_cmp_zero(cg_sec, r2, 8); // cset %s, %s\n", reg32[r_result], is_unsigned ? "cs" : "vs
                        asm_cset(cg_sec, r_result, ARM64_NE); // subs %s, %s, %s
                    } else {
                        int r3 = alloc_reg();
                        (void)0 /* FIXME: NEON */;
                        asm_mul_reg_reg(cg_sec, ra, ra, 8); // cset %s, %s\n", reg32[r_result], is_unsigned ? "cc" : "vs
                        asm_sar_imm(cg_sec, r3, 8, 63); // umulh %s, %s, %s
                        asm_cmp_reg_reg(cg_sec, r2, r3, 8); // mul %s, %s, %s
                        asm_cset(cg_sec, r_result, ARM64_NE); // cmp %s, #0
                        free_reg(r3);
                    }
                } else {
                    if (is_unsigned) {
                        (void)0 /* FIXME: NEON */;
                        asm_shr_imm(cg_sec, r2, 8, 32); // cset %s, ne
                        asm_cmp_zero(cg_sec, r2, 8); // smulh %s, %s, %s
                        asm_cset(cg_sec, r_result, ARM64_NE); // mul %s, %s, %s
                    } else {
                        int r3 = alloc_reg();
                        (void)0 /* FIXME: NEON */;
                        asm_sar_imm(cg_sec, r2, 8, 31); // asr %s, %s, #63
                        asm_shr_imm(cg_sec, r3, 8, 32); // cmp %s, %s
                        asm_cmp_reg_reg(cg_sec, r2, r3, 8); // cset %s, ne
                        asm_cset(cg_sec, r_result, ARM64_NE); // umull %s, %s, %s
                        free_reg(r3);
                    }
                }
                free_reg(r2);
            }
            if (argres && !is_mul_overflow_p) {
                int rr = gen_addr(argres);
                asm_str_reg_off(cg_sec, ra, rr, 8, 0); // str %s, [%s]
                free_reg(rr);
            }
            free_reg(ra);
            free_reg(rb);
            return r_result;
        }
#endif
#ifndef ARCH_ARM64
        if (is_add_overflow || is_sub_overflow || is_mul_overflow || is_mul_overflow_p) {
            Node *arga = node->args;
            Node *argb = arga ? arga->next : NULL;
            Node *argres = argb ? argb->next : NULL;
            // x86-64: use add/sub/mul tracking unsigned overflow (carry flag)
            int ra = gen(arga);
            int rb = gen(argb);
            int sz = arga && arga->ty && arga->ty->size > 4 ? 8 : 4;
            if (is_add_overflow) {
                (void)0 /* FIXME: generic 2op: %s %s, %s */;
                asm_setcc(cg_sec, 0, X86_C); // %s %s, %s\n", sz == 8 ? "add" : "add
                if (argres) {
                    int rr = gen_addr(argres);
                    (void)0 /* FIXME: mov indirect/mem */;
                    free_reg(rr);
                }
            } else if (is_sub_overflow) {
                (void)0 /* FIXME: sub 2op */;
                asm_setcc(cg_sec, 0, X86_C); // setc %%al
                if (argres) {
                    int rr = gen_addr(argres);
                    (void)0 /* FIXME: mov indirect/mem */;
                    free_reg(rr);
                }
            } else if (is_mul_overflow || is_mul_overflow_p) {
                int r2 = alloc_reg();
                if (sz == 8) {
                    asm_mov_reg_reg(cg_sec, 0, ra, 8); // mov %s, (%s)
                    (void)0 /* FIXME: mul 1op */;
                    asm_mov_reg_reg(cg_sec, ra, 0, 8); // sub %s, %s
                    asm_mov_reg_reg(cg_sec, r2, 2, 8); // setc %%al
                } else {
                    asm_mov_reg_reg(cg_sec, 0, ra, 4); // mov %s, (%s)
                    (void)0 /* FIXME: mul 1op */;
                    asm_mov_reg_reg(cg_sec, ra, 0, 4); // movq %s, %%rax
                    asm_mov_reg_reg(cg_sec, r2, 2, 4); // mul %s
                }
                asm_cmp_zero(cg_sec, r2, sz); // movq %%rax, %s
                asm_setcc(cg_sec, 0, X86_NE); // movq %%rdx, %s
                free_reg(r2);
                if (argres && !is_mul_overflow_p) {
                    int rr = gen_addr(argres);
                    (void)0 /* FIXME: mov indirect/mem */;
                    free_reg(rr);
                }
            }
            int r_result = alloc_reg();
            asm_movzx(cg_sec, r_result, 0, 4, 1); // movl %s, %%eax
            free_reg(ra);
            free_reg(rb);
            return r_result;
        }
#endif
    }

    // Inline expansion for common libc builtins (x86_64 only for now)
    bool skip_builtins = false;
#ifdef ARCH_ARM64
    skip_builtins = true;
#endif
    if (call_target && !has_hidden_retbuf && !skip_builtins) {
        // Inline expansion for common libc builtins
        bool is_memset = strcmp(call_target, "memset") == 0 ||
            strcmp(call_target, "__builtin_memset") == 0;
        bool is_memcpy = strcmp(call_target, "memcpy") == 0 ||
            strcmp(call_target, "__builtin_memcpy") == 0;
        bool is_memcmp = strcmp(call_target, "memcmp") == 0 ||
            strcmp(call_target, "__builtin_memcmp") == 0;
        bool is_strlen = strcmp(call_target, "strlen") == 0 ||
            strcmp(call_target, "__builtin_strlen") == 0;
        bool is_strcmp = strcmp(call_target, "strcmp") == 0 ||
            strcmp(call_target, "__builtin_strcmp") == 0;
        bool is_strchr = strcmp(call_target, "strchr") == 0 ||
            strcmp(call_target, "__builtin_strchr") == 0;

        if (is_memset || is_memcpy) {
            Node *dst = node->args;
            Node *v2 = dst ? dst->next : NULL;
            Node *len = v2 ? v2->next : NULL;
            if (dst && v2 && len && !len->next) {
                int r = alloc_reg();
                int dst_r = gen(dst);
                int v2_r = gen(v2);
                int len_r = gen(len);
                asm_mov_reg_reg(cg_sec, dst_r, r, 8); // mov %s, %s

                asm_nop(cg_sec); // cld
                asm_push_phy(cg_sec, X86_RDI); // pushq %%rdi
                if (is_memcpy) asm_push_phy(cg_sec, X86_RSI); // pushq %%rsi
                asm_push_phy(cg_sec, X86_RCX); // pushq %%rcx
                asm_mov_reg_reg(cg_sec, 7, dst_r, 8); // movq %s, %%rdi
                asm_mov_reg_reg(cg_sec, 1, len_r, 8); // movq %s, %%rcx
                if (is_memset) {
                    x86_movzx(cg_sec, 4, 1, X86_RAX, CG_X86_REG(v2_r)); // movzbl %s, %%eax
                    x86_rep_prefix(cg_sec); // rep stosb
                    secbuf_emit8(cg_sec, 0xaa); /* stosb */ /* movq %s, %%rsi\n */
                } else {
                    asm_mov_reg_reg(cg_sec, 6, v2_r, 8); // rep movsb
                    x86_rep_prefix(cg_sec); // popq %%rcx
                    secbuf_emit8(cg_sec, 0xa4); /* movsb */ /* popq %%rsi\n */
                }
                asm_pop_phy(cg_sec, X86_RCX); // popq %%rdi
                if (is_memcpy) asm_pop_phy(cg_sec, X86_RSI); // mov %s, %s
                asm_pop_phy(cg_sec, X86_RDI); // cld

                free_reg(dst_r);
                free_reg(v2_r);
                free_reg(len_r);

                return r;
            }
        }

        if (is_memcmp) {
            Node *src1 = node->args;
            Node *src2 = src1 ? src1->next : NULL;
            Node *len = src2 ? src2->next : NULL;
            if (src1 && src2 && len && !len->next) {
                int s1_r = gen(src1);
                int s2_r = gen(src2);
                int len_r = gen(len);

                asm_nop(cg_sec); // cld
                asm_push_phy(cg_sec, X86_RDI); // pushq %%rdi
                asm_push_phy(cg_sec, X86_RSI); // pushq %%rsi
                asm_push_phy(cg_sec, X86_RCX); // pushq %%rcx
                asm_mov_reg_reg(cg_sec, 7, s1_r, 8); // movq %s, %%rdi
                asm_mov_reg_reg(cg_sec, 6, s2_r, 8); // movq %s, %%rsi
                asm_mov_reg_reg(cg_sec, 1, len_r, 8); // movq %s, %%rcx
                (void)0 /* FIXME: rep */;
                asm_jcc_label(cg_sec, X86_NE); // repe cmpsb
                asm_movl_zero(cg_sec, 0); // jne .L.memcmp_diff.%d
                asm_jmp_label(cg_sec); // xorl %%eax, %%eax
                (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
                (void)0 /* FIXME: string op */;
                (void)0 /* FIXME: string op */;
                asm_sub_reg_reg(cg_sec, 0, 1, 4); // jmp .L.memcmp_end.%d
                (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
                asm_pop_phy(cg_sec, X86_RCX); // .L.memcmp_diff.%d:
                asm_pop_phy(cg_sec, X86_RSI); // movsbl -1(%%rdi), %%eax
                asm_pop_phy(cg_sec, X86_RDI); // movsbl -1(%%rsi), %%ecx

                free_reg(s1_r);
                free_reg(s2_r);
                free_reg(len_r);

                int r = alloc_reg();
                asm_mov_reg_reg(cg_sec, r, 0, 4); // subl %%ecx, %%eax
                return r;
            }
        }

        if (is_strlen) {
            Node *str = node->args;
            if (str && !str->next) {
                int str_r = gen(str);

                asm_nop(cg_sec); // cld
                asm_push_phy(cg_sec, X86_RDI); // pushq %%rdi
                asm_push_phy(cg_sec, X86_RCX); // pushq %%rcx
                asm_mov_reg_reg(cg_sec, 7, str_r, 8); // movq %s, %%rdi
                x86_xor_rr(cg_sec, 1, X86_RAX, X86_RAX); // xorb %%al, %%al
                x86_mov_ri(cg_sec, 8, X86_RCX, -1); // movq $-1, %%rcx
                x86_repne_prefix(cg_sec); // repne scasb
                secbuf_emit8(cg_sec, 0xae); /* scasb */ /* notq %%rcx\n */
                (void)0 /* FIXME: notq */;
                asm_dec(cg_sec, 1, 8); // decq %%rcx
                asm_mov_reg_reg(cg_sec, 0, 1, 8); // movq %%rcx, %%rax
                asm_pop_phy(cg_sec, X86_RCX); // popq %%rcx
                asm_pop_phy(cg_sec, X86_RDI); // popq %%rdi

                free_reg(str_r);

                int r = alloc_reg();
                asm_mov_reg_reg(cg_sec, r, 0, 8); // movq %%rax, %s
                return r;
            }
        }

        if (is_strcmp) {
            Node *s1 = node->args;
            Node *s2 = s1 ? s1->next : NULL;
            if (s1 && s2 && !s2->next) {
                int r = gen(s1);
                int r2 = gen(s2);
                int cl = ++rcc_label_count;
                asm_push_phy(cg_sec, X86_RDI); // pushq %%rdi
                asm_push_phy(cg_sec, X86_RSI); // pushq %%rsi
                asm_mov_reg_reg(cg_sec, 7, r, 8); // movq %s, %%rdi
                asm_mov_reg_reg(cg_sec, 6, r2, 8); // movq %s, %%rsi
                (void)0 /* FIXME: label .L.xxx.cl */;
                (void)0 /* FIXME: sized mov */;
                (void)0 /* FIXME: cmp variant */;
                asm_jcc_label(cg_sec, X86_NE); // .L.strcmp_loop.%d:
                (void)0 /* FIXME: testb */;
                asm_jcc_label(cg_sec, X86_Z); // movb (%%rdi), %%al
                asm_inc(cg_sec, 7, 8); // cmpb (%%rsi), %%al
                asm_inc(cg_sec, 6, 8); // jne .L.strcmp_diff.%d
                asm_jmp_label(cg_sec); // testb %%al, %%al
                (void)0 /* FIXME: label .L.xxx.cl */;
                asm_movzx(cg_sec, 0, 0, 4, 1); // jz .L.strcmp_eq.%d
                (void)0 /* FIXME: indirect ext */;
                asm_sub_reg_reg(cg_sec, 0, 1, 4); // incq %%rdi
                asm_jmp_label(cg_sec); // incq %%rsi
                (void)0 /* FIXME: label .L.xxx.cl */;
                asm_movl_zero(cg_sec, 0); // jmp .L.strcmp_loop.%d
                (void)0 /* FIXME: label .L.xxx.cl */;
                (void)0 /* FIXME: label .L.xxx.cl */;
                asm_pop_phy(cg_sec, X86_RSI); // .L.strcmp_diff.%d:
                asm_pop_phy(cg_sec, X86_RDI); // movzbl %%al, %%eax
                free_reg(r);
                free_reg(r2);
                int ret = alloc_reg();
                asm_mov_reg_reg(cg_sec, ret, 0, 4); // movzbl (%%rsi), %%ecx
                return ret;
            }
        }

        if (is_strchr) {
            Node *s = node->args;
            Node *c = s ? s->next : NULL;
            if (s && c && !c->next) {
                int sr = gen(s);
                int cr = gen(c);
                int cl = ++rcc_label_count;
                asm_push_phy(cg_sec, X86_RDI); // pushq %%rdi
                asm_push_phy(cg_sec, X86_RCX); // pushq %%rcx
                asm_mov_reg_reg(cg_sec, 7, sr, 8); // movq %s, %%rdi
                x86_movzx(cg_sec, 4, 1, X86_RAX, CG_X86_REG(cr)); // movzbl %s, %%eax
                cg_def_label(format(".L.strchr.%d", cl)); // .L.strchr_loop.%d:
                x86_cmp_rm(cg_sec, 1, X86_RAX, x86_mem(CG_X86_REG(7), 0)); // cmpb %%al, (%%rdi)
                asm_jcc_label(cg_sec, X86_E); // je .L.strchr_found.%d
                x86_cmp_ri(cg_sec, 1, X86_RAX, 0); // cmpb $0, (%%rdi)
                asm_jcc_label(cg_sec, X86_E); // je .L.strchr_null.%d
                asm_inc(cg_sec, 7, 8); // incq %%rdi
                asm_jmp_label(cg_sec); // jmp .L.strchr_loop.%d
                cg_def_label(format(".L.strchr_end.%d", cl)); // .L.strchr_found.%d:
                asm_mov_reg_reg(cg_sec, 0, 7, 8); // movq %%rdi, %%rax
                asm_jmp_label(cg_sec); // jmp .L.strchr_end.%d
                cg_def_label(format(".L.strchr_ret.%d", cl)); // .L.strchr_null.%d:
                asm_movl_zero(cg_sec, 0); // xorl %%eax, %%eax
                cg_def_label(format(".L.strchr_done.%d", cl)); // .L.strchr_end.%d:
                asm_pop_phy(cg_sec, X86_RCX); // popq %%rcx
                asm_pop_phy(cg_sec, X86_RDI); // popq %%rdi
                free_reg(sr);
                free_reg(cr);
                int ret = alloc_reg();
                asm_mov_reg_reg(cg_sec, ret, 0, 8); // movq %%rax, %s
                return ret;
            }
        }
    }

#ifdef ARCH_ARM64
    // Inline alloca: directly adjust sp without any register save/restore
    // (save/restore around a bl __rcc_alloca would use the stack, which alloca moves)
    if (call_target && strcmp(call_target, "alloca") == 0) {
        alloca_needed = true;
        fn_uses_alloca = true;
        int ra = gen(node->args);
        // Round up to 16 bytes and adjust sp
        asm_add_imm(cg_sec, ra, 8 if 1 == "1" else 4, 15); // add %s, %s, #15
        asm_and_imm(cg_sec, ra, 8 if 1 == "1" else 4, ~15ULL); // and %s, %s, #-16
        asm_sub_reg_reg(cg_sec, 31, ra, 8 if 1 == "1" else 4); // sub sp, sp, %s
        asm_mov_reg_reg(cg_sec, ra, 0, 8 if 1 == "1" else 4); // mov %s, sp
        // Save current sp/ptr to the alloca var slot
        if (node->args && node->args->ty) {
            // caller uses return value from ra
        }
        return ra;
    }
#endif

#ifdef _WIN32
    int fixed_reg_args = nargs + (has_hidden_retbuf ? 1 : 0);
    int stack_args = fixed_reg_args > max_gp_args ? fixed_reg_args - max_gp_args : 0;
    int stack_pad = (stack_args & 1) ? 8 : 0;
    int stack_reserve = stack_args > 0 ? shadow_space + stack_args * 8 + stack_pad : 0;
#elif defined(ARCH_ARM64)
    // AAPCS64: 8 GP + 8 FP arg registers
    // Linux: variadic floats go in both FP and GP regs
    // Apple: variadic args always on stack
    int gp_reg_args = 0;
    int fp_reg_args = 0;
    int stack_args = 0;
    Type *fn_type = (node->lhs && node->lhs->ty && node->lhs->ty->kind == TY_PTR)
        ? node->lhs->ty->base
        : NULL;
    bool is_variadic = fn_type && fn_type->kind == TY_FUNC && fn_type->is_variadic;
    int named_count = 0;
    if (fn_type && fn_type->kind == TY_FUNC)
        for (Type *t = fn_type->param_types; t; t = t->param_next)
            named_count++;
    for (int i = 0; i < nargs; i++) {
        arg_regs[i] = -1;
        arg_sizes[i] = argv[i]->ty->size;
        arg_is_float[i] = is_flonum(argv[i]->ty);
        arg_gp_idx[i] = -1;
        arg_fp_idx[i] = -1;
        arg_stack_idx[i] = -1;
        arg_hfa_count[i] = 0;
        arg_hfa_elem_size[i] = 0;
        bool is_named = (i < named_count);
        if (!arg_is_float[i] && (argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION))
            arg_hfa_count[i] = arm64_hfa_count(argv[i]->ty, &arg_hfa_elem_size[i]);
        if (arg_is_float[i]) {
            // Long double (> 8 bytes): pass in q-register (SIMD), not d-register or stack
            if (arg_sizes[i] > 8) {
                if (fp_reg_args < max_fp_args)
                    arg_fp_idx[i] = fp_reg_args++;
                else
                    arg_stack_idx[i] = stack_args++;
                continue;
            }
            if (is_variadic && !is_named) {
#if defined(__APPLE__)
                // Apple ARM64: variadic args always on the stack
                arg_stack_idx[i] = stack_args++;
#else
                // Linux AAPCS64: variadic floats in FP regs only
                // (GP copy for unnamed FP args is optional; glibc does not require it)
                if (fp_reg_args < max_fp_args)
                    arg_fp_idx[i] = fp_reg_args++;
                else if (gp_reg_args < max_gp_args)
                    arg_gp_idx[i] = gp_reg_args++;
                else
                    arg_stack_idx[i] = stack_args++;
#endif
            } else if (fp_reg_args < max_fp_args) {
                arg_fp_idx[i] = fp_reg_args++;
                // Named FP args in variadic functions go in FP regs only (no GP copy)
            } else if (is_variadic && arg_sizes[i] > 8) {
                arg_stack_idx[i] = stack_args++;
            } else if (gp_reg_args < max_gp_args) {
                arg_gp_idx[i] = gp_reg_args++;
            } else {
                arg_stack_idx[i] = stack_args++;
            }
            continue;
        }
        if (arg_hfa_count[i] > 0 && !is_variadic) {
            if (fp_reg_args + arg_hfa_count[i] <= max_fp_args) {
                arg_fp_idx[i] = fp_reg_args;
                fp_reg_args += arg_hfa_count[i];
            } else {
                arg_stack_idx[i] = stack_args;
                stack_args += arg_hfa_count[i];
            }
            continue;
        }
#if defined(__APPLE__)
        if (is_variadic && !is_named) {
            arg_stack_idx[i] = stack_args++;
            continue;
        }
#endif
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8) {
            // Large struct > 8 bytes passed by pointer via GP reg or stack
            if (gp_reg_args < max_gp_args)
                arg_gp_idx[i] = gp_reg_args++;
            else
                arg_stack_idx[i] = stack_args++;
            continue;
        }
        if (gp_reg_args < max_gp_args)
            arg_gp_idx[i] = gp_reg_args++;
        else
            arg_stack_idx[i] = stack_args++;
    }
    //int stack_pad = (stack_args & 1) ? 8 : 0;
    //int stack_reserve = stack_args > 0 ? stack_args * 8 + stack_pad : 0;
#else
    int gp_reg_args = has_hidden_retbuf ? 1 : 0;
    int fp_reg_args = 0;
    int stack_args = 0;
    Type *fn_type = (node->lhs && node->lhs->ty && node->lhs->ty->kind == TY_PTR)
        ? node->lhs->ty->base
        : NULL;
    bool is_variadic = fn_type && fn_type->kind == TY_FUNC && fn_type->is_variadic;
    int named_count = 0;
    if (fn_type && fn_type->kind == TY_FUNC)
        for (Type *t = fn_type->param_types; t; t = t->param_next)
            named_count++;
    for (int i = 0; i < nargs; i++) {
        arg_regs[i] = -1;
        arg_sizes[i] = (argv[i]->ty->kind == TY_ARRAY) ? 8 : argv[i]->ty->size;
        arg_is_float[i] = is_flonum(argv[i]->ty);
        arg_gp_idx[i] = -1;
        arg_fp_idx[i] = -1;
        arg_stack_idx[i] = -1;
        bool is_named = (i < named_count);

        if (arg_is_float[i]) {
            if (argv[i]->ty->kind == TY_LDOUBLE && is_variadic && !is_named) {
                if (stack_args & 1)
                    stack_args++;
                arg_stack_idx[i] = stack_args;
                stack_args += 2;
                continue;
            }
            if (fp_reg_args < max_fp_args) {
                arg_fp_idx[i] = fp_reg_args++;
            } else {
                arg_stack_idx[i] = stack_args++;
            }
            continue;
        }

        if (gp_reg_args < max_gp_args)
            arg_gp_idx[i] = gp_reg_args++;
        else
            arg_stack_idx[i] = stack_args++;
    }

    int stack_pad = (stack_args & 1) ? 8 : 0;
    int stack_reserve = stack_args > 0 ? shadow_space + stack_args * 8 + stack_pad : 0;
#endif

#ifdef ARCH_ARM64
    // ARM64: evaluate args for register passing, track stack args
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if (arg_hfa_count[i] > 0 || ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)) {
            int addr = gen_addr(argv[i]);
            if (addr < 0) {
                // Non-lvalue struct/union (e.g. cast to struct): allocate temp slot and store value
                int sz = argv[i]->ty->size;
                int alloc = (sz + 15) & ~15;
                fn_struct_ret_off += alloc;
                if (fn_struct_ret_off > fn_struct_ret_total)
                    fn_struct_ret_total = fn_struct_ret_off;
                int tmp_slot = current_fn_stack_size + fn_struct_ret_off;
                addr = alloc_reg();
                emit_mov_imm64("x16", (uint64_t)tmp_slot);
                (void)0 /* FIXME: sub with phy reg */;
                // Zero out the slot (handles up to 16 bytes; larger structs need a loop)
                for (int zb = 0; zb < alloc; zb += 16)
                    (void)0 /* FIXME: ldp/stp */;
                // Store the computed value into the first bytes of the temp slot
                int val = gen(argv[i]);
                if (val >= 0) {
                    int vsz = argv[i]->ty->size < 8 ? argv[i]->ty->size : 8;
                    if (vsz == 4)
                        asm_str_reg_off(cg_sec, val, addr, 4, 0); // sub %s, %s, x16
                    else
                        asm_str_reg_off(cg_sec, val, addr, 8, 0); // stp xzr, xzr, [%s, #%d]
                    free_reg(val);
                }
            }
            arg_regs[i] = addr;
        } else
            arg_regs[i] = gen(argv[i]);
    }

    // Allocate one block for caller-saved regs + stack args.
    // Layout (from low sp upward):
    //   [sp + 0 .. stack_args*8-1]        : stack args (callee reads via x29+16+idx*8)
    //   [sp + stack_args*8 .. total-1]    : saved caller-saved regs
    // This keeps stack args at the standard AAPCS64 location relative to sp_at_call.
    int arm64_saved_mask = used_regs & 63;
    int sv_count = __builtin_popcount(arm64_saved_mask);

    if (sv_count > 0 || stack_args > 0) {
        int total = (sv_count + stack_args) * 8;
        total = (total + 15) & ~15;
        asm_sub_imm(cg_sec, 31, 8 if 1 == "1" else 4, total); // sub %s, %s, #%d
    }

    // Save caller-saved regs ABOVE stack args area
    int sv_off = 0;
    for (int i = 0; i < 6; i++) {
        if (arm64_saved_mask & (1 << i)) {
            (void)0 /* FIXME: ldr/str phy/off */;
            sv_off++;
        }
    }

    // Push stack args at sp+idx*8 (callee reads from x29+16+idx*8 = sp+idx*8)
    for (int i = nargs - 1; i >= 0; i--) {
        if (arg_stack_idx[i] < 0)
            continue;
        int r;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            r = gen_addr(argv[i]);
        else
            r = gen(argv[i]);
        (void)0 /* FIXME: ldr/str phy/off */;
        free_reg(r);
    }

    int temp_ret_reg = -1;
    int temp_ret_slot = 0;
    if (has_hidden_retbuf) {
        if (hidden_ret_reg == -1) {
            temp_ret_reg = alloc_reg();
            int alloc = (node->ty->size + 15) & ~15;
            fn_struct_ret_off += alloc;
            if (fn_struct_ret_off > fn_struct_ret_total)
                fn_struct_ret_total = fn_struct_ret_off;
            temp_ret_slot = current_fn_stack_size + fn_struct_ret_off;
            if (temp_ret_slot <= 4095)
                asm_add_imm(cg_sec, temp_ret_reg, 8, FRAME_PTR); // add %s, %s, #%d
            else {
                int v = temp_ret_slot;
                asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // mov x16, #%d
                v >>= 16;
                int s = 16;
                while (v) {
                    asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // movk x16, #%d, lsl #%d
                    v >>= 16;
                    s += 16;
                }
                (void)0 /* FIXME: sub with phy reg */;
            }
            hidden_ret_reg = temp_ret_reg;
        }
        (void)0 /* FIXME: mov phy */;
    }

    // Pre-pass: long double args.
    // For named long double params in rcc-compiled callees: pass as double (fmov d_idx, reg).
    // For unnamed variadic long double args (e.g. printf "%Lf"): use quad format for ABI compat.
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if (arg_is_float[i] && arg_sizes[i] > 8 && arg_fp_idx[i] >= 0) {
            bool is_named_arg = (i < named_count);
            if (is_named_arg) {
                // Named long double param: rcc callee reads as double from d_idx
                (void)0 /* FIXME: fmov */;
            } else {
                // Unnamed variadic long double: pass as IEEE 754 quad (128-bit) for ABI
                int cl = ++rcc_label_count;
                char *vr = reg64[arg_regs[i]];
                asm_cmp_zero(cg_sec, arg_regs[i], 8); // fmov %s, %s
                asm_jcc_label(cg_sec, ARM64_EQ); // cmp %s, #0
                (void)0 /* FIXME: NEON */;
                (void)0 /* FIXME: mov phy */;
                (void)0 /* FIXME: add phy */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: and variant */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: orr phy */;
                secbuf_emit32le(cg_sec, arm64_asr_imm(1, 17, CG_ARM_REG(arg_regs[i]), 63)); // b.eq .L.quad_z.%d
                (void)0 /* FIXME: and variant */;
                (void)0 /* FIXME: lsl/lsr phy */;
                (void)0 /* FIXME: orr phy */;
                asm_jmp_label(cg_sec); // ubfx x17, %s, #52, #11
                (void)0 /* FIXME: label .L.xxx.cl */;
                (void)0 /* FIXME: mov phy */;
                (void)0 /* FIXME: mov phy */;
                (void)0 /* FIXME: label .L.xxx.cl */;
                (void)0 /* FIXME: NEON */;
                (void)0 /* FIXME: NEON */;
            }
            free_reg(arg_regs[i]);
        }
    }

    // Move evaluated args into arg registers
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if (arg_is_float[i] && arg_sizes[i] > 8 && arg_fp_idx[i] >= 0) {
            continue;
        }
        if (arg_hfa_count[i] > 0 && arg_fp_idx[i] >= 0) {
            for (int j = 0; j < arg_hfa_count[i]; j++) {
                int off = j * arg_hfa_elem_size[i];
                if (arg_hfa_elem_size[i] == 4)
                    (void)0 /* FIXME: ldr/str phy/off */;
                else
                    (void)0 /* FIXME: ldr/str phy/off */;
            }
            free_reg(arg_regs[i]);
            continue;
        }
        // Variadic small HFA struct (size<=8): gen_addr gave address, but GP register
        // must hold the VALUE (va_arg reads it directly, not via pointer).
        if (arg_hfa_count[i] > 0 && arg_gp_idx[i] >= 0 && arg_fp_idx[i] < 0 && arg_sizes[i] <= 8) {
            if (arg_sizes[i] == 4)
                secbuf_emit32le(cg_sec, arm64_ldr_uoff(0, arg_gp_idx[i], CG_ARM_REG(arg_regs[i]), 0)); // ldr w16, [%s]\n  mov %s, x16
            else
                secbuf_emit32le(cg_sec, arm64_ldr_uoff(1, arg_gp_idx[i], CG_ARM_REG(arg_regs[i]), 0)); // ldr %s, [%s]
            free_reg(arg_regs[i]);
            continue;
        }
        if (arg_is_float[i] && arg_fp_idx[i] >= 0) {
            (void)0 /* FIXME: fmov */;
            // Convert double->float only if callee param is known float,
            // not for variadic args (which stay promoted to double)
            bool var_double = (is_variadic && i >= named_count) ||
                (fn_type && (fn_type->is_oldstyle || !fn_type->param_types));
            if (!var_double) {
                if (argv[i]->ty->size == 4)
                    (void)0 /* FIXME: float op */;
                else if (fn_type && fn_type->param_types && i < named_count) {
                    Type *pt = fn_type->param_types;
                    for (int j = 0; j < i && pt; j++) pt = pt->param_next;
                    if (pt && pt->kind == TY_FLOAT)
                        (void)0 /* FIXME: float op */;
                }
            }
            if (arg_gp_idx[i] >= 0)
                secbuf_emit32le(cg_sec, arm64_add_reg(1, arg_gp_idx[i], 31, CG_ARM_REG(arg_regs[i]), ARM64_LSL, 0)); // fmov %s, %s
        } else if (arg_is_float[i] && arg_gp_idx[i] >= 0) {
            secbuf_emit32le(cg_sec, arm64_add_reg(1, arg_gp_idx[i], 31, CG_ARM_REG(arg_regs[i]), ARM64_LSL, 0)); // fcvt s%d, %s
        } else if (arg_sizes[i] == 1 || arg_sizes[i] == 2) {
            secbuf_emit32le(cg_sec, arm64_add_reg(1, arg_gp_idx[i], 31, CG_ARM_REG(arg_regs[i]), ARM64_LSL, 0)); // fcvt s%d, %s
        } else if (arg_sizes[i] == 4) {
            secbuf_emit32le(cg_sec, arm64_add_reg(0, arg_gp_idx[i], 31, CG_ARM_REG(arg_regs[i]), ARM64_LSL, 0)); // mov %s, %s
        } else {
            secbuf_emit32le(cg_sec, arm64_add_reg(1, arg_gp_idx[i], 31, CG_ARM_REG(arg_regs[i]), ARM64_LSL, 0)); // mov %s, %s
        }
        free_reg(arg_regs[i]);
    }

    if (call_target) {
        if (strcmp(call_target, "alloca") == 0) {
            alloca_needed = true;
            fn_uses_alloca = true;
            emit_direct_call("__rcc_alloca");
        } else {
            emit_direct_call(call_target);
        }
    } else {
        int callee = gen(node->lhs);
        asm_call_reg(cg_sec, r); // blr %s
        free_reg(callee);
    }

    // Restore caller-saved registers from above-stack-args area
    if (arm64_saved_mask) {
        int sv = 0;
        for (int i = 0; i < 6; i++) {
            if (arm64_saved_mask & (1 << i)) {
                (void)0 /* FIXME: ldr/str phy/off */;
                sv++;
            }
        }
    }

    // Restore sp
    if (sv_count > 0 || stack_args > 0) {
        int total = (sv_count + stack_args) * 8;
        total = (total + 15) & ~15;
        asm_add_imm(cg_sec, 31, 8 if 1 == "1" else 4, total); // add %s, %s, #%d
    }

    if (has_hidden_retbuf) {
        if (temp_ret_reg != -1) {
            if (temp_ret_slot <= 4095)
                asm_add_imm(cg_sec, temp_ret_reg, 8, FRAME_PTR); // add %s, %s, #%d
            else {
                int v = temp_ret_slot;
                asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // mov x16, #%d
                v >>= 16;
                int s = 16;
                while (v) {
                    asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // movk x16, #%d, lsl #%d
                    v >>= 16;
                    s += 16;
                }
                (void)0 /* FIXME: sub with phy reg */;
            }
        }
        return temp_ret_reg != -1 ? temp_ret_reg : hidden_ret_reg;
    }

    int r = alloc_reg();
    if (node->ty && is_flonum(node->ty)) {
        if (node->ty->kind == TY_FLOAT)
            (void)0 /* arm64 fcvt d0,s0 */;
        (void)0 /* FIXME: fmov */;
    } else {
        asm_mov_reg_reg(cg_sec, r, 0, 8 if 1 == "1" else 4); // fcvt d0, s0
    }
    return r;
#else
    // === x86_64 (Windows + Linux) calling convention ===
    int saved_scratch = used_regs & 3;
    if ((saved_scratch & 1) && hidden_ret_reg != 0) {
        asm_mov_phyreg_rbp(cg_sec, X86_R10, 8, spill_offset(0)); // fmov %s, d0
        // Keep r10 marked as in-use so alloc_reg() doesn't reuse it for the
        // hidden ret buffer, which would overwrite a caller's live arg value.
    }
    if ((saved_scratch & 2) && hidden_ret_reg != 1) {
        asm_mov_phyreg_rbp(cg_sec, X86_R11, 8, spill_offset(1)); // mov %s, x0
        // Same for r11.
    }

    int callee_reg = -1;
    if (!call_target) {
        callee_reg = gen(node->lhs);
    }

#ifdef _WIN32
    int reg_nargs = nargs < max_gp_args - (has_hidden_retbuf ? 1 : 0) ? nargs : max_gp_args - (has_hidden_retbuf ? 1 : 0);
    for (int i = 0; i < reg_nargs; i++) {
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            arg_regs[i] = gen_addr(argv[i]);
        else
            arg_regs[i] = gen(argv[i]);
        arg_sizes[i] = (argv[i]->ty->kind == TY_ARRAY) ? 8 : argv[i]->ty->size;
        arg_is_float[i] = is_flonum(argv[i]->ty);
    }

    if (stack_reserve > 0)
        asm_sub_imm(cg_sec, 4, 8, stack_reserve); // movq (%s), %s

    for (int i = nargs - 1; i >= reg_nargs; i--) {
        int r = gen(argv[i]);
        int off = (i - reg_nargs) * 8;
        if (is_flonum(argv[i]->ty)) {
            x86_mov_mr(cg_sec, 8, x86_mem(X86_RSP, off), CG_X86_REG(r)); // subq $%d, %%rsp
        } else {
            if (argv[i]->ty->size == 1)
                asm_movzx(cg_sec, r, r, 4, 1); // movq %s, %d(%%rsp)
            else if (argv[i]->ty->size == 4)
                asm_mov_reg_reg(cg_sec, r, r, 4); // movq %s, %d(%%rsp)
            x86_mov_mr(cg_sec, 8, x86_mem(X86_RSP, off), CG_X86_REG(r)); // movzbl %s, %s
        }
        free_reg(r);
    }
#else
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8) {
            int addr = gen_addr(argv[i]);
            if (addr < 0) {
                // Non-lvalue (e.g. cast to struct): allocate temp, evaluate, store
                int alloc = (argv[i]->ty->size + 7) & ~7;
                fn_struct_ret_off += alloc;
                if (fn_struct_ret_off > fn_struct_ret_total)
                    fn_struct_ret_total = fn_struct_ret_off;
                int tmp_slot = current_fn_stack_size + fn_struct_ret_off;
                addr = alloc_reg();
                asm_lea_rbp_reg(cg_sec, addr, 8, tmp_slot); // lea -%d(%%rbp), %s
                int val = gen(argv[i]);
                (void)0 /* FIXME: mov indirect/mem */;
                free_reg(val);
            }
            arg_regs[i] = addr;
        } else
            arg_regs[i] = gen(argv[i]);
    }

    // Bitmask of scratch registers still needed for register-passed args.
    // Stack arg computation below may free and reuse these via the spill
    // mechanism; re-marking them as in-use forces a proper spill/restore
    // instead of a silent overwrite that would lose the pre-computed value.
    int reg_arg_mask = 0;
    for (int i = 0; i < nargs; i++)
        if (arg_regs[i] >= 0 && arg_stack_idx[i] < 0)
            reg_arg_mask |= (1 << arg_regs[i]);

    if (stack_reserve > 0)
        asm_sub_imm(cg_sec, 4, 8, stack_reserve); // subq $%d, %%rsp

    for (int i = nargs - 1; i >= 0; i--) {
        if (arg_stack_idx[i] < 0)
            continue;
        used_regs |= reg_arg_mask; // keep pre-computed reg args live
        if (argv[i]->ty->kind == TY_LDOUBLE) {
            int r = gen(argv[i]);
            x86_mov_mr(cg_sec, 8, x86_mem(X86_RSP, arg_stack_idx[i] * 8), CG_X86_REG(r)); // movq %s, %d(%%rsp)
            X86Mem mld = x86_mem(X86_RSP, arg_stack_idx[i] * 8); // fldl %d(%%rsp)
            x86_fstpt_m(cg_sec, mld); // fstpt %d(%%rsp)
            free_reg(r);
            used_regs |= reg_arg_mask; // restore any bits cleared by free_reg
            continue;
        }
        int r;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            r = gen_addr(argv[i]);
        else
            r = gen(argv[i]);
        if (is_flonum(argv[i]->ty)) {
            x86_mov_mr(cg_sec, 8, x86_mem(X86_RSP, arg_stack_idx[i] * 8), CG_X86_REG(r)); // movq %s, %d(%%rsp)
        } else {
            if (argv[i]->ty->is_unsigned)
                zero_extend_to(r, argv[i]->ty->size, 8);
            else
                sign_extend_to(r, argv[i]->ty->size, 8);
            x86_mov_mr(cg_sec, 8, x86_mem(X86_RSP, arg_stack_idx[i] * 8), CG_X86_REG(r)); // movq %s, %d(%%rsp)
        }
        free_reg(r);
        used_regs |= reg_arg_mask; // restore any bits cleared by free_reg
    }
#endif

    int xmm_args = 0;
#ifdef _WIN32
    for (int i = 0; i < reg_nargs; i++) {
        if (arg_is_float[i]) xmm_args++;
    }
#else
    xmm_args = fp_reg_args;
#endif

    int temp_ret_reg = -1;
    int temp_ret_slot = 0;
    if (has_hidden_retbuf) {
        if (hidden_ret_reg == -1) {
            temp_ret_reg = alloc_reg();
            int alloc = (node->ty->size + 15) & ~15;
            fn_struct_ret_off += alloc;
            if (fn_struct_ret_off > fn_struct_ret_total)
                fn_struct_ret_total = fn_struct_ret_off;
            temp_ret_slot = current_fn_stack_size + fn_struct_ret_off;
            asm_lea_rbp_reg(cg_sec, temp_ret_reg, 8, temp_ret_slot); // lea -%d(%%rbp), %s
            hidden_ret_reg = temp_ret_reg;
        }
        (void)0 /* FIXME: mov reg64[hidden_ret_reg], argreg64[0] */;
    }

#ifdef _WIN32
    for (int i = 0; i < reg_nargs; i++) {
        int argi = i + (has_hidden_retbuf ? 1 : 0);
        if (arg_is_float[i]) {
            x86_movq_r_xmm(cg_sec, (X86XmmReg)(arg_fp_idx[i]), CG_X86_REG(arg_regs[i])); // mov %s, %s
            x86_mov_rr(cg_sec, 8, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movq %s, 0(%%rsp)
        } else if (arg_sizes[i] == 1) {
            if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                x86_movsx(cg_sec, 4, 1, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movq %s, %s
            else
                x86_movzx(cg_sec, 4, 1, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // mov %s, %s
        } else if (arg_sizes[i] == 2) {
            if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                x86_movsx(cg_sec, 4, 2, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movsbl %s, %s
            else
                x86_movzx(cg_sec, 4, 2, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movzbl %s, %s
        } else if (arg_sizes[i] == 4) {
            if (argv[i]->ty->is_unsigned)
                x86_mov_rr(cg_sec, 4, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movswl %s, %s
            else
                x86_movsx(cg_sec, 8, 4, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // movzwl %s, %s
        } else {
            x86_mov_rr(cg_sec, 8, cg_x86_argreg[argi], CG_X86_REG(arg_regs[i])); // mov %s, %s
        }
        free_reg(arg_regs[i]);
    }
#else
    // Two-pass placement: reg64[7]=="%rsi"==argreg64[1], so any arg whose scratch
    // register is rsi must be placed before the arg that writes to rsi, otherwise
    // the write clobbers the source value.  Pass 0: rsi-sourced args.  Pass 1: rest.
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nargs; i++) {
            if (arg_stack_idx[i] >= 0)
                continue;
            bool rsi_src = (!arg_is_float[i] && arg_regs[i] == 7);
            if (pass == 0 && !rsi_src) continue;
            if (pass == 1 && rsi_src) continue;
            if (arg_is_float[i]) {
                x86_movq_r_xmm(cg_sec, (X86XmmReg)(arg_fp_idx[i]), CG_X86_REG(arg_regs[i])); // movq %s, %s
            } else if (arg_sizes[i] == 1) {
                if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                    x86_movsx(cg_sec, 4, 1, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // movsbl %s, %s
                else
                    x86_movzx(cg_sec, 4, 1, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // movzbl %s, %s
            } else if (arg_sizes[i] == 2) {
                if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                    x86_movsx(cg_sec, 4, 2, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // movswl %s, %s
                else
                    x86_movzx(cg_sec, 4, 2, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // movzwl %s, %s
            } else if (arg_sizes[i] == 4) {
                x86_mov_rr(cg_sec, 4, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // mov %s, %s
            } else {
                x86_mov_rr(cg_sec, 8, cg_x86_argreg[arg_gp_idx[i]], CG_X86_REG(arg_regs[i])); // movslq %s, %s
            }
            free_reg(arg_regs[i]);
        }
    } // end two-pass
#endif

    (void)0 /* FIXME: movl imm */;
    if (call_target) {
        if (strcmp(call_target, "alloca") == 0) {
            alloca_needed = true;
            fn_uses_alloca = true;
            emit_direct_call("__rcc_alloca");
        } else {
            emit_direct_call(call_target);
        }
    } else {
        asm_call_reg(cg_sec, callee_reg); // call *%s
        free_reg(callee_reg);
    }

    if (stack_reserve > 0)
        (void)0 /* FIXME: sized alu op */;

    if ((saved_scratch & 2) && hidden_ret_reg != 1) {
        used_regs |= 2;
        asm_mov_rbp_phyreg(cg_sec, X86_R11, 8, spill_offset(1)); // addq $%d, %%rsp
    }
    if ((saved_scratch & 1) && hidden_ret_reg != 0) {
        used_regs |= 1;
        asm_mov_rbp_phyreg(cg_sec, X86_R10, 8, spill_offset(0)); // movq -%d(%%rbp), %%r11
    }

    if (has_hidden_retbuf) {
        if (temp_ret_reg != -1) {
            // temp_ret_reg (r10/r11) may have been clobbered by the callee; reload
            // the frame-relative address — it is always valid.
            asm_lea_rbp_reg(cg_sec, temp_ret_reg, 8, temp_ret_slot); // movq -%d(%%rbp), %%r10
        }
        return temp_ret_reg != -1 ? temp_ret_reg : hidden_ret_reg;
    }

    int r = alloc_reg();
    if (node->ty && is_flonum(node->ty)) {
#ifndef ARCH_ARM64
        if (node->ty->kind == TY_FLOAT)
            asm_cvtss2sd(cg_sec); // lea -%d(%%rbp), %s
#endif
        (void)0 /* TODO: movq to/from xmm */;
    } else {
        asm_mov_reg_reg(cg_sec, r, 0, 8); // movq (%s), %s
    }
    return r;
#endif
}

// Map any register name to a physical register ID (for peephole optimization)
static int phys_reg_id(const char *s) {
    if (s[0] == '%') s++;
#ifndef ARCH_ARM64
    if (!strncmp(s, "r10", 3)) return 10;
    if (!strncmp(s, "r11", 3)) return 11;
    if (!strncmp(s, "r12", 3)) return 12;
    if (!strncmp(s, "r13", 3)) return 13;
    if (!strncmp(s, "r14", 3)) return 14;
    if (!strncmp(s, "r15", 3)) return 15;
    if (!strncmp(s, "rax", 3) || !strncmp(s, "eax", 3) || !strcmp(s, "al") || !strcmp(s, "ax")) return 0;
    if (!strncmp(s, "rcx", 3) || !strncmp(s, "ecx", 3) || !strcmp(s, "cl") || !strcmp(s, "cx")) return 1;
    if (!strncmp(s, "rdx", 3) || !strncmp(s, "edx", 3) || !strcmp(s, "dl") || !strcmp(s, "dx")) return 2;
    if (!strncmp(s, "rbx", 3) || !strncmp(s, "ebx", 3) || !strcmp(s, "bl") || !strcmp(s, "bx")) return 3;
    if (!strncmp(s, "rsp", 3) || !strncmp(s, "esp", 3)) return 4;
    if (!strncmp(s, "rbp", 3) || !strncmp(s, "ebp", 3)) return 5;
    if (!strncmp(s, "rsi", 3) || !strncmp(s, "esi", 3) || !strcmp(s, "sil") || !strcmp(s, "si")) return 6;
    if (!strncmp(s, "r8", 2) && (s[2] == '\0' || s[2] == 'd' || s[2] == 'w' || s[2] == 'b')) return 8;
    if (!strncmp(s, "r9", 2) && (s[2] == '\0' || s[2] == 'd' || s[2] == 'w' || s[2] == 'b')) return 9;
#else
    if ((s[0] == 'x' || s[0] == 'w') && s[1] >= '0' && s[1] <= '9') {
        int n = s[1] - '0';
        if (s[2] >= '0' && s[2] <= '9') n = n * 10 + (s[2] - '0');
        if (n >= 0 && n <= 30 && (s[2] == '\0' || (s[2] >= '0' && s[2] <= '9' && s[3] == '\0')))
            return n;
    }
    if (!strcmp(s, "xzr") || !strcmp(s, "wzr")) return 31;
#endif
    return -1;
}

#ifndef ARCH_ARM64
static int same_phys(const char *a, const char *b) {
    int ia = phys_reg_id(a), ib = phys_reg_id(b);
    return ia >= 0 && ia == ib;
}
#endif

static int add_float_literal(double val, int size) {
    FloatLit *fl = arena_alloc(sizeof(FloatLit));
    fl->id = float_lit_count++;
    fl->val = val;
    fl->size = size;
    fl->next = float_lits;
    float_lits = fl;
    return fl->id;
}

// Names that GAS treats as keywords in Intel syntax (segment registers and
// expression operators).  Global symbols with these names need a safe alias.
static bool is_asm_reserved(const char *name) {
    static const char *kw[] = {
        "cs", "ds", "es", "fs", "gs", "ss",
        "%al", "%ah", "%ax", "%eax", "%rax",
        "%bl", "%bh", "%bx", "%ebx", "%rbx",
        "%cl", "%ch", "%cx", "%ecx", "%rcx",
        "%dl", "%dh", "%dx", "%edx", "%rdx",
        "%sil", "%si", "%esi", "%rsi",
        "%dil", "%di", "%edi", "%rdi",
        "%bpl", "%bp", "%ebp", "%rbp",
        "%spl", "%sp", "%esp", "%rsp",
        "%r8", "%r8b", "%r8w", "%r8d",
        "%r9", "%r9b", "%r9w", "%r9d",
        "%r10", "%r10b", "%r10w", "%r10d",
        "%r11", "%r11b", "%r11w", "%r11d",
        "%r12", "%r12b", "%r12w", "%r12d",
        "%r13", "%r13b", "%r13w", "%r13d",
        "%r14", "%r14b", "%r14w", "%r14d",
        "%r15", "%r15b", "%r15w", "%r15d",
        "and", "or", "not", "xor", "shl", "shr", "mod",
        "eq", "ne", "lt", "le", "ge", "gt",
        NULL};
    for (int i = 0; kw[i]; i++)
        if (strcmp(name, kw[i]) == 0) return true;
    return false;
}

static char *var_label(LVar *var) {
    if (var->asm_name) return var->asm_name;
    if (is_asm_reserved(var->name)) return format(".L_rcc_%s", var->name);
    return var->name;
}

static char *reg(int r, int size) {
#ifdef ARCH_ARM64
    if (r < 0 || r > 11)
        error("invalid register %d, arm64 has only 12", r);
    if (size == 8) return reg64[r];
    return reg32[r]; // arm64: wN for 1/2/4 bytes
#else
    if (r < 0 || r > 7)
        error("invalid register %d, x86_64 has only 8", r);
    if (size == 1) return reg8[r];
    if (size == 2) return reg16[r];
    if (size == 4) return reg32[r];
    return reg64[r];
#endif
}

#ifdef ARCH_ARM64
// Emit mov reg, #imm64 handling any size (movz + movk)
static void emit_mov_imm64(const char *reg, uint64_t val) {
    bool is_w = (reg[0] == 'w');
    int sf = is_w ? 0 : 1;
    int rd = atoi(reg + 1);
    asm_movz(cg_sec, rd, sf, (uint16_t)(val & 0xffff), 0); // mov %s, #%llu
    val >>= 16;
    int shift = 16;
    while (val) {
        asm_movk(cg_sec, rd, sf, (uint16_t)(val & 0xffff), shift); // movk %s, #%llu, lsl #%d
        val >>= 16;
        shift += 16;
    }
}

// Emit mov reg, #imm for a signed 32-bit immediate, choosing 32- or 64-bit encoding
static void emit_mov_imm(const char *reg, int imm) {
    bool is_w = (reg[0] == 'w');
    int sf = is_w ? 0 : 1;
    int rd = atoi(reg + 1);
    uint64_t val = is_w ? (uint64_t)(uint32_t)imm : (uint64_t)(int64_t)(int32_t)imm;
    asm_movz(cg_sec, rd, sf, (uint16_t)(val & 0xffff), 0); // mov %s, #%llu
    val >>= 16;
    int shift = 16;
    int max_shift = is_w ? 16 : 48;
    while (val && shift <= max_shift) {
        asm_movk(cg_sec, rd, sf, (uint16_t)(val & 0xffff), shift); // movk %s, #%llu, lsl #%d
        val >>= 16;
        shift += 16;
    }
}

// Emit adrp+add pair for label address, with platform-appropriate syntax
// Linux: adrp reg, label / add reg, reg, :lo12:label
// Darwin: adrp reg, label@PAGE / add reg, reg, label@PAGEOFF
static void emit_adrp_add(const char *reg, const char *label) {
    bool is_w = (reg[0] == 'w');
    int rd = atoi(reg + 1);
    (void)is_w;
    size_t adrp_off = cg_sec->len;
    asm_adrp(cg_sec, rd); // adrp %s, %s@GOTPAGE
    int sidx = objfile_find_sym(cg_obj, label);
    if (sidx < 0)
        sidx = objfile_add_sym(cg_obj, label, SEC_UNDEF, 0, 0, SB_GLOBAL, ST_NOTYPE);
    objfile_add_reloc(cg_obj, SEC_TEXT, adrp_off, sidx, R_AARCH64_ADR_PREL_PG_HI21, 0);
    size_t add_off = cg_sec->len;
    secbuf_emit32le(cg_sec, arm64_add_imm(1, rd, rd, 0, 0)); // ldr %s, [%s, %s@GOTPAGEOFF]
    objfile_add_reloc(cg_obj, SEC_TEXT, add_off, sidx, R_AARCH64_ADD_ABS_LO12_NC, 0);
}

// GOT-based address load for weak symbols: undefined weak → NULL, defined → address.
// Required on Linux ARM64; Darwin already uses GOT in emit_adrp_add.
static void emit_adrp_got(const char *reg, const char *label) {
    bool is_w = (reg[0] == 'w');
    int rd = atoi(reg + 1);
    (void)is_w;
    emit_adrp_add(reg, label);
    size_t ldr_off = cg_sec->len;
    secbuf_emit32le(cg_sec, arm64_ldr_uoff(1, rd, rd, 0)); // adrp %s, %s
    int sidx = objfile_find_sym(cg_obj, label);
    if (sidx >= 0)
        objfile_add_reloc(cg_obj, SEC_TEXT, ldr_off, sidx, R_AARCH64_LD64_GOT_LO12_NC, 0);
}
#endif

// Emit load/store-safe address for [x29, #-offset] when offset > 255
// Returns register holding the address (must be freed by caller)
#if 0
static int emit_stack_addr(int offset) {
#ifdef ARCH_ARM64
    int ta = alloc_reg();
    if (offset <= 4095)
        asm_sub_imm(cg_sec, ta, 8, FRAME_PTR); // sub %s, %s, #%d
    else {
        int v = offset;
        asm_mov_imm(cg_sec, ta, 8, v & 0xffff); // mov %s, #%d
        v >>= 16;
        int s = 16;
        while (v) {
            asm_movk(cg_sec, ta, 1, (uint16_t)(v & 0xffff), s); // movk %s, #%d, lsl #%d
            v >>= 16;
            s += 16;
        }
        asm_sub_reg_reg(cg_sec, ta, ta, 8 if 1=="1" else 4); // sub %s, %s, %s
    }
    return ta;
#else
    int ta = alloc_reg();
    asm_lea_rbp_reg(cg_sec, ta, 8, offset); // lea -%d(%%rbp), %s
    return ta;
#endif
}
#endif

static int op_size(Type *ty) {
    if (is_integer(ty) && ty->size < 4)
        return 4;
    if (ty->size > 8)
        return 8;
    return ty->size;
}

static Type *promote_int(Type *ty) {
    if (ty && is_integer(ty) && ty->size < 4)
        return ty_int;
    return ty;
}

#ifndef ARCH_ARM64
static char size_suffix(int sz) {
    if (sz == 1) return 'b';
    if (sz == 2) return 'w';
    if (sz == 4) return 'l';
    return 'q';
}
#endif

static bool use_unsigned(Type *ty) {
    return ty && is_integer(ty) && ty->is_unsigned;
}

static bool use_unsigned_cmp(Node *node) {
    Type *lhs = promote_int(node->lhs->ty);
    Type *rhs = promote_int(node->rhs->ty);
    int size = lhs->size > rhs->size ? lhs->size : rhs->size;
    if (size < 4)
        size = 4;
    return get_integer_type(size, lhs->is_unsigned || rhs->is_unsigned)->is_unsigned;
}

static void sign_extend_to(int r, int from_size, int to_size) {
    if (to_size <= from_size)
        return;
    if (to_size == 8) {
        if (from_size == 4)
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 8, 4); // sxtw %s, %s
#else
            asm_movsx(cg_sec, r, r, 8, 4); // movslq %s, %s
#endif
        else if (from_size == 2)
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 4, 2); // sxth %s, %s
#else
            asm_movsx(cg_sec, r, r, 8, 2); // movswq %s, %s
#endif
        else if (from_size == 1)
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 4, 1); // sxtb %s, %s
#else
            asm_movsx(cg_sec, r, r, 8, 1); // movsbq %s, %s
#endif
        else
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 8, 4); // sxtw %s, %s
#else
            asm_movsx(cg_sec, r, r, 8, 4); // movslq %s, %s
#endif
    } else if (to_size == 4) {
        if (from_size == 2)
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 4, 2); // sxth %s, %s
#else
            asm_movsx(cg_sec, r, r, 4, 2); // movswl %s, %s
#endif
        else if (from_size == 1)
#ifdef ARCH_ARM64
            asm_movsx(cg_sec, r, r, 4, 1); // sxtb %s, %s
#else
            asm_movsx(cg_sec, r, r, 4, 1); // movsbl %s, %s
#endif
        else
            asm_mov_reg_reg(cg_sec, r, r, 4); // mov %s, %s
    }
}

static void zero_extend_to(int r, int from_size, int to_size) {
    if (to_size <= from_size)
        return;
    if (to_size == 8) {
        if (from_size == 4)
            asm_mov_reg_reg(cg_sec, r, r, 4); // mov %s, %s
        else if (from_size == 2)
#ifdef ARCH_ARM64
            asm_movzx(cg_sec, r, r, 4, 2); // uxth %s, %s
#else
            asm_movzx(cg_sec, r, r, 4, 2); // movzwl %s, %s
#endif
        else if (from_size == 1)
#ifdef ARCH_ARM64
            asm_movzx(cg_sec, r, r, 4, 1); // uxtb %s, %s
#else
            asm_movzx(cg_sec, r, r, 4, 1); // movzbl %s, %s
#endif
        else
            asm_mov_reg_reg(cg_sec, r, r, 4); // mov %s, %s
    } else if (to_size == 4) {
        if (from_size == 2)
#ifdef ARCH_ARM64
            asm_movzx(cg_sec, r, r, 4, 2); // uxth %s, %s
#else
            asm_movzx(cg_sec, r, r, 4, 2); // movzwl %s, %s
#endif
        else if (from_size == 1)
#ifdef ARCH_ARM64
            asm_movzx(cg_sec, r, r, 4, 1); // uxtb %s, %s
#else
            asm_movzx(cg_sec, r, r, 4, 1); // movzbl %s, %s
#endif
        else
            asm_mov_reg_reg(cg_sec, r, r, 4); // mov %s, %s
    }
}

#ifdef ARCH_ARM64
// Sentinels for base register index in emit_store / emit_load:
// >= 0: virtual register index, CG_ARM_REG maps to physical register
// ARM64_BASE_FP (-29): frame pointer (x29)
// ARM64_BASE_SP (-31): stack pointer (x31)
#define ARM64_BASE_FP (-29)
#define ARM64_BASE_SP (-31)
#define ARM64_BASE_X16 (-16)

// Get physical ARM64 register number from base index
static int arm64_base_phys(int base) {
    if (base == ARM64_BASE_FP) return 29;
    if (base == ARM64_BASE_SP) return 31;
    if (base == ARM64_BASE_X16) return 16;
    return CG_ARM_REG(base);
}

// Check if base register matches destination virtual register
static bool arm64_base_conflicts(int base, int r) {
    return base >= 0 && base == r;
}

static void emit_store(Type *ty, int src, int base, int off) {
    int sz = ty->size;
    int bp = arm64_base_phys(base);
    if (off == 0) {
        if (sz == 1) asm_str_reg_off(cg_sec, src, bp, 1, 0); // strb %s, %s
        else if (sz == 2)
            asm_str_reg_off(cg_sec, src, bp, 2, 0); // strh %s, %s
        else if (sz == 4)
            asm_str_reg_off(cg_sec, src, bp, 8 if 0 == "1" else 4, 0); // str %s, %s
        else
            asm_str_reg_off(cg_sec, src, bp, 8 if 1 == "1" else 4, 0); // str %s, %s
    } else if (off > -256 && off < 256) {
        if (sz == 4) asm_stur(cg_sec, src, bp, 0, off); // mov %s, #%d
        else
            asm_stur(cg_sec, src, bp, 1, off); // movk %s, #%d, lsl #%d
    } else {
        int ta = alloc_reg();
        if (off < 0) {
            int u = -off;
            asm_mov_imm(cg_sec, ta, 8, u); // sub %s, %s, %s
            asm_add_reg_reg(cg_sec, ta, ta, 8 if 1 == "1" else 4); // add %s, %s, %s
        } else {
            asm_mov_imm(cg_sec, ta, 8, off); // strb %s, [%s]
            asm_add_reg_reg(cg_sec, ta, ta, 8 if 1 == "1" else 4); // strh %s, [%s]
        }
        secbuf_emit32le(cg_sec, arm64_str_uoff(sz == 8 ? 1 : 0, CG_ARM_REG(src), CG_ARM_REG(ta), 0)); // str %s, [%s]
        free_reg(ta);
    }
}

static void emit_store_offset(Type *ty, int r, int base, int offset) {
    int sz = ty->size;
    int bp = arm64_base_phys(base);
    int off = offset;
    if (off > -256 && off < 256 && sz >= 4) {
        secbuf_emit32le(cg_sec, arm64_stur(sz == 8 ? 1 : 0, CG_ARM_REG(r), bp, off)); // str %s, [%s]
        return;
    }
    int abs_off = off < 0 ? -off : off;
    int ta = alloc_reg();
    asm_mov_imm(cg_sec, ta, 8, abs_off & 0xffff); // strb %s, [%s, #%d]
    int v = abs_off >> 16;
    int s = 16;
    while (v) {
        asm_movk(cg_sec, ta, 1, (uint16_t)(v & 0xffff), s); // strh %s, [%s, #%d]
        v >>= 16;
        s += 16;
    }
    if (off < 0)
        asm_sub_reg_reg(cg_sec, ta, ta, 8 if 1 == "1" else 4); // str %s, [%s, #%d]
    else
        asm_add_reg_reg(cg_sec, ta, ta, 8 if 1 == "1" else 4); // str %s, [%s, #%d]
    if (sz == 1) secbuf_emit32le(cg_sec, arm64_strb_uoff(CG_ARM_REG(r), CG_ARM_REG(ta), 0)); // mov x16, %s
    else if (sz == 2)
        secbuf_emit32le(cg_sec, arm64_strh_uoff(CG_ARM_REG(r), CG_ARM_REG(ta), 0)); // mov %s, %s
    else if (sz == 4)
        secbuf_emit32le(cg_sec, arm64_str_uoff(0, CG_ARM_REG(r), CG_ARM_REG(ta), 0)); // sub %s, %s, #%d
    else
        secbuf_emit32le(cg_sec, arm64_str_uoff(1, CG_ARM_REG(r), CG_ARM_REG(ta), 0)); // mov %s, #%d
    free_reg(ta);
}
#endif

static void emit_load(Type *ty, int r, int base, int off) {
#ifdef ARCH_ARM64
    int bp = arm64_base_phys(base);
    // ARM64: ldr wN,[xN] is CONSTRAINED UNPREDICTABLE — avoid base==dest
    if (arm64_base_conflicts(base, r)) {
        secbuf_emit32le(cg_sec, arm64_add_reg(1, 16, 31, bp, ARM64_LSL, 0)); // movk %s, #%d, lsl #%d
        bp = 16; // x16 scratch
    }
    if (ty->size == 1) {
        secbuf_emit32le(cg_sec, arm64_ldrb_imm(CG_ARM_REG(r), bp, (int32_t)off)); // sub %s, %s, %s
    } else if (ty->size == 2) {
        secbuf_emit32le(cg_sec, arm64_ldrh_imm(CG_ARM_REG(r), bp, (int32_t)off)); // %s %s, [%s]\n", ty->is_unsigned ? "ldrb" : "ldrsb
    } else if (ty->size == 4) {
        secbuf_emit32le(cg_sec, arm64_ldr_imm(0, CG_ARM_REG(r), bp, (int32_t)off, false)); // %s %s, [%s]\n", ty->is_unsigned ? "ldrh" : "ldrsh
    } else {
        secbuf_emit32le(cg_sec, arm64_ldr_imm(1, CG_ARM_REG(r), bp, (int32_t)off, false)); // ldr %s, [%s]
    }
#else
#ifndef X86_BASE_RBP
#define X86_BASE_RBP (-1)
#endif
    // x86_64: base >= 0 = virtual reg, X86_BASE_RBP = rbp-relative /* ldr %s, [%s]\n */
    int sz = op_size(ty);
    if (base == X86_BASE_RBP) {
        X86Reg xbp = CG_X86_FP;
        if (ty->size == 1) {
            if (ty->is_unsigned)
                x86_movzx_rm(cg_sec, 4, 1, CG_X86_REG(r), x86_mem(xbp, off)); // %s %s, %s\n", ty->is_unsigned ? "ldrb" : "ldrsb
            else
                x86_movsx_rm(cg_sec, 4, 1, CG_X86_REG(r), x86_mem(xbp, off)); // %s %s, %s\n", ty->is_unsigned ? "ldrh" : "ldrsh
        } else if (ty->size == 2) {
            if (ty->is_unsigned)
                x86_movzx_rm(cg_sec, 4, 2, CG_X86_REG(r), x86_mem(xbp, off)); // ldr %s, %s
            else
                x86_movsx_rm(cg_sec, 4, 2, CG_X86_REG(r), x86_mem(xbp, off)); // ldr %s, %s
        } else {
            asm_mov_rbp_reg(cg_sec, r, sz, off); // %s %s, %s\n", ty->is_unsigned ? "movzbl" : "movsbl
        }
    } else {
        X86Mem m = {CG_X86_REG(base), X86_NOREG, 1, (int64_t)off};
        if (ty->size == 1) {
            if (ty->is_unsigned)
                x86_movzx_rm(cg_sec, 4, 1, CG_X86_REG(r), m); // %s %s, %s\n", ty->is_unsigned ? "movzwl" : "movswl
            else
                x86_movsx_rm(cg_sec, 4, 1, CG_X86_REG(r), m); // movl %s, %s
        } else if (ty->size == 2) {
            if (ty->is_unsigned)
                x86_movzx_rm(cg_sec, 4, 2, CG_X86_REG(r), m); // movq %s, %s
            else
                x86_movsx_rm(cg_sec, 4, 2, CG_X86_REG(r), m); // mov %s, %s
        } else {
            x86_mov_rm(cg_sec, sz, CG_X86_REG(r), m); // uxth %s, %s
        }
    }
    if (sz == 8 && ty->size < 4)
        asm_movzx(cg_sec, r, r, 4, ty->size); // movzwl %s, %s
#endif
}

static int alloc_reg(void) {
    for (int i = 0; i < NUM_REGS; i++) {
        if ((used_regs & (1 << i)) == 0) {
            used_regs |= (1 << i);
            ever_used_regs |= (1 << i);
            return i;
        }
    }
    // All registers are in use. Spill the register with the highest index
    // (least likely to be referenced by outer callers right now).
    for (int i = NUM_REGS - 1; i >= 0; i--) {
        if (used_regs & (1 << i)) {
            if (opt_W) {
                if (reg_owner[i])
                    fprintf(stderr, "\033[1;33mwarning:\033[0m spilling %s (%s) to stack in %s\n", reg64[i], reg_owner[i], current_fn);
                else
                    fprintf(stderr, "\033[1;33mwarning:\033[0m spilling %s to stack in %s\n", reg64[i], current_fn);
            }
#ifdef ARCH_ARM64
            asm_str_fp_imm(cg_sec, i, 8, spill_offset(i)); // str %s, [%s, #-%d]
#else
            asm_mov_reg_rbp(cg_sec, i, 8, spill_offset(i)); // mov %s, -%d(%%rbp)
#endif
            spilled_regs |= (1 << i);
            spill_count++;
            used_regs &= ~(1 << i);
            used_regs |= (1 << i); // reclaim for new value
            ever_used_regs |= (1 << i);
            return i;
        }
    }
    error("Register exhaustion");
    return 0;
}

static void free_reg(int i) {
    if (spilled_regs & (1 << i)) {
#ifdef ARCH_ARM64
        asm_ldr_fp_imm(cg_sec, i, 8, spill_offset(i)); // ldr %s, [%s, #-%d]
#else
        asm_mov_rbp_reg(cg_sec, i, 8, spill_offset(i)); // mov -%d(%%rbp), %s
#endif
        spilled_regs &= ~(1 << i);
    }
    used_regs &= ~(1 << i);
    reg_owner[i] = NULL;
}

static int gen(Node *node);

#ifdef ARCH_ARM64
// Minimal ARM64 inline-asm template validator.
// Reports rcc-style errors for invalid mnemonics and range violations
// so that test 139 gets our messages instead of gas messages.
static void arm64_validate_asm_template(const char *tmpl, Token *tok) {
    // Known ARM64 mnemonics (comprehensive but not exhaustive)
    static const char *const known[] = {
        "add", "adds", "sub", "subs", "neg", "negs", "mul", "madd", "msub", "mneg",
        "div", "udiv", "sdiv", "smull", "umull", "smulh", "umulh",
        "and", "ands", "orr", "eor", "bic", "bics", "orn", "eon",
        "lsl", "lsr", "asr", "ror", "extr",
        "mov", "movz", "movk", "movn",
        "adrp", "adr",
        "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrsw",
        "ldur", "ldurb", "ldurh", "ldursb", "ldursh", "ldursw",
        "str", "strb", "strh", "stur", "sturb", "sturh",
        "ldp", "stp", "ldnp", "stnp",
        "b", "bl", "blr", "br", "ret",
        "cbz", "cbnz", "tbz", "tbnz",
        "b.eq", "b.ne", "b.lt", "b.le", "b.gt", "b.ge",
        "b.cc", "b.cs", "b.hi", "b.ls", "b.mi", "b.pl", "b.vs", "b.vc",
        "b.lo", "b.hs", "b.al", "b.nv", "b.eq", "beq", "bne", "blt", "ble", "bgt", "bge",
        "mrs", "msr", "nop", "brk", "svc", "hlt",
        "dmb", "dsb", "isb",
        "csel", "cset", "csetm", "csinc", "csinv", "csneg",
        "clz", "cls", "rbit", "rev", "rev16", "rev32",
        "sbfm", "ubfm", "bfm", "sbfx", "ubfx", "bfi", "bfxil",
        "sxth", "sxtw", "sxtb", "uxtb", "uxth",
        "fmov", "fadd", "fsub", "fmul", "fdiv", "fneg", "fabs", "fsqrt",
        "fcmp", "fccmp", "fcmpe", "fccmpe",
        "fcvt", "fcvtzu", "fcvtzs", "scvtf", "ucvtf", "fcvtas", "fcvtau",
        "fcvtms", "fcvtmu", "fcvtns", "fcvtnu", "fcvtps", "fcvtpu",
        "fmax", "fmin", "fmaxnm", "fminnm",
        "paciasp", "autiasp", "pacibsp", "autibsp", "xpaclri",
        "prfm", "prefetch",
        "eret", "drps", "eor",
        "lda", "ldaex", "stl", "stlex", "stlr", "ldar", "ldaxr", "stlxr",
        "ldxr", "stxr", "ldaxp", "stlxp", "ldxp", "stxp",
        "clrex", "yield", "wfe", "wfi", "sev", "sevl",
        "hint", "sys", "sysl", "at", "dc", "ic", "tlbi", "msr", "mrs",
        NULL};

    const char *p = tmpl;
    while (*p) {
        // skip whitespace / separators
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ';') p++;
        if (!*p) break;

        // skip operand substitutions (%N, %w0, %[name], etc.)
        if (*p == '%') {
            while (*p && *p != ';' && *p != '\n') p++;
            continue;
        }

        // Extract mnemonic (first word)
        char mnem[64];
        int mlen = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != ';' && *p != '\n' && mlen < 63)
            mnem[mlen++] = tolower((unsigned char)*p++);
        mnem[mlen] = '\0';
        if (!mlen) continue;

        // Skip to rest of instruction
        while (*p == ' ' || *p == '\t') p++;
        const char *operands = p;

        // Validate mnemonic
        bool found = false;
        for (int j = 0; known[j]; j++) {
            if (strcmp(mnem, known[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            error_tok_simple(tok, "ARM64 instruction '%s' not implemented", mnem);
            // skip rest of line and continue
            while (*p && *p != ';' && *p != '\n') p++;
            continue;
        }

        // --- Specific instruction validation ---

        // LSL/LSR/ASR immediate shift range
        if ((strcmp(mnem, "lsl") == 0 || strcmp(mnem, "lsr") == 0 || strcmp(mnem, "asr") == 0) &&
            operands[0] && operands[0] != '%') {
            bool is32 = (tolower((unsigned char)operands[0]) == 'w');
            const char *hash = NULL;
            for (const char *q = operands; *q && *q != ';' && *q != '\n'; q++)
                if (*q == '#') {
                    hash = q + 1;
                    break;
                }
            if (hash) {
                long val = strtol(hash, NULL, 0);
                int maxshift = is32 ? 31 : 63;
                if (val < 0 || val > maxshift)
                    error_tok_simple(tok, "shift immediate out of range");
            }
        }

        // MRS: validate system register name
        if (strcmp(mnem, "mrs") == 0) {
            static const char *const sysregs[] = {
                "fpcr", "fpsr", "nzcv", "daif", "currentel", "spsel",
                "midr_el1", "mpidr_el1", "id_aa64pfr0_el1", "id_aa64pfr1_el1",
                "id_aa64dfr0_el1", "id_aa64dfr1_el1", "id_aa64isar0_el1",
                "id_aa64isar1_el1", "id_aa64mmfr0_el1", "id_aa64mmfr1_el1",
                "sctlr_el1", "tcr_el1", "ttbr0_el1", "ttbr1_el1", "mair_el1",
                "tpidr_el0", "tpidrro_el0", "tpidr_el1",
                "sp_el0", "elr_el1", "spsr_el1",
                "cntkctl_el1", "cntpct_el0", "cntvct_el0", "cntfrq_el0",
                "pmcr_el0", "pmcntenset_el0", "pmccntr_el0", "pmintenset_el1",
                "esr_el1", "far_el1", "par_el1", "afsr0_el1", "afsr1_el1",
                "revidr_el1", "aidr_el1", "csselr_el1", "clidr_el1", "ctr_el0",
                "dczid_el0", "isr_el1", "vbar_el1", "rvbar_el1", "rmr_el1",
                "contextidr_el1", "tpidr_el0", "tpidrro_el0", "amair_el1",
                NULL};
            const char *comma = strchr(operands, ',');
            if (comma) {
                const char *r = comma + 1;
                while (*r == ' ' || *r == '\t') r++;
                char sysreg[64];
                int sl = 0;
                while (*r && *r != ' ' && *r != '\t' && *r != '\n' && *r != ';' && sl < 63)
                    sysreg[sl++] = tolower((unsigned char)*r++);
                sysreg[sl] = '\0';
                if (*sysreg && *sysreg != '%') {
                    bool ok = false;
                    for (int j = 0; sysregs[j]; j++)
                        if (strcmp(sysreg, sysregs[j]) == 0) {
                            ok = true;
                            break;
                        }
                    if (!ok)
                        error_tok_simple(tok, "unsupported system register");
                }
            }
        }

        // MSR: validate system register name (first operand)
        if (strcmp(mnem, "msr") == 0) {
            static const char *const sysregs_msr[] = {
                "fpcr", "fpsr", "nzcv", "daif", "currentel", "spsel",
                "sctlr_el1", "tcr_el1", "ttbr0_el1", "ttbr1_el1", "mair_el1",
                "tpidr_el0", "tpidr_el1", "vbar_el1", "contextidr_el1", "amair_el1",
                "cntkctl_el1", "pmcr_el0", "pmcntenset_el0", "pmintenset_el1",
                NULL};
            char sysreg[64];
            int sl = 0;
            const char *r = operands;
            while (*r == ' ' || *r == '\t') r++;
            while (*r && *r != ',' && *r != ' ' && *r != '\t' && *r != '\n' && sl < 63)
                sysreg[sl++] = tolower((unsigned char)*r++);
            sysreg[sl] = '\0';
            (void)sysregs_msr;
            (void)sysreg; // validated by gas; don't double-error
        }

        // DMB/DSB: validate barrier option
        if (strcmp(mnem, "dmb") == 0 || strcmp(mnem, "dsb") == 0) {
            static const char *const opts[] = {
                "sy", "st", "ld", "ish", "ishst", "ishld",
                "nsh", "nshst", "nshld", "osh", "oshst", "oshld",
                "full", NULL};
            char opt[32];
            int ol = 0;
            const char *r = operands;
            while (*r == ' ' || *r == '\t') r++;
            while (*r && *r != ' ' && *r != '\t' && *r != '\n' && *r != ';' && ol < 31)
                opt[ol++] = tolower((unsigned char)*r++);
            opt[ol] = '\0';
            if (*opt && *opt != '%') {
                // allow numeric
                bool ok = (*opt >= '0' && *opt <= '9');
                for (int j = 0; !ok && opts[j]; j++)
                    if (strcmp(opt, opts[j]) == 0) ok = true;
                if (!ok)
                    error_tok_simple(tok, "invalid operand '%s'", opt);
            }
        }

        // ADD/SUB: check for missing third operand (when operands are literal)
        if ((strcmp(mnem, "add") == 0 || strcmp(mnem, "sub") == 0) &&
            operands[0] && operands[0] != '%') {
            int ncommas = 0;
            for (const char *q = operands; *q && *q != ';' && *q != '\n'; q++)
                if (*q == ',') ncommas++;
            if (ncommas < 2)
                error_tok_simple(tok, "missing third operand");
        }

        // MOVZ/MOVK: check immediate value and shift
        if (strcmp(mnem, "movz") == 0 || strcmp(mnem, "movk") == 0) {
            const char *hash = NULL;
            for (const char *q = operands; *q && *q != ';' && *q != '\n'; q++)
                if (*q == '#') {
                    hash = q + 1;
                    break;
                }
            if (hash) {
                long val = strtol(hash, NULL, 0);
                if (val < 0 || val > 65535)
                    error_tok_simple(tok, "move wide immediate out of range");
                // Check for lsl shift
                const char *comma2 = NULL;
                {
                    // Find comma after the first arg (dest reg)
                    const char *q = operands;
                    int nc = 0;
                    while (*q && *q != ';' && *q != '\n') {
                        if (*q == ',') {
                            nc++;
                            if (nc == 2) {
                                comma2 = q;
                                break;
                            }
                        }
                        q++;
                    }
                }
                if (comma2) {
                    const char *h2 = strchr(comma2, '#');
                    if (h2) {
                        long shift = strtol(h2 + 1, NULL, 0);
                        if (shift != 0 && shift != 16 && shift != 32 && shift != 48)
                            error_tok_simple(tok, "move wide shift out of range");
                    }
                }
            }
        }

        // Skip rest of this instruction
        while (*p && *p != ';' && *p != '\n') p++;
    }
}
#endif // ARCH_ARM64

#ifdef ARCH_ARM64
// Try to extract an integer constant from a node (traversing casts).
// Returns true and sets *val if the node reduces to a compile-time constant.
static bool try_const_int(Node *n, int64_t *val) {
    while (n && n->kind == ND_CAST)
        n = n->lhs;
    if (n && n->kind == ND_NUM) {
        *val = n->val;
        return true;
    }
    return false;
}
#endif // ARCH_ARM64

// Generate code to compute the absolute address of an lvalue.
static int gen_addr(Node *node) {
    switch (node->kind) {
    case ND_LVAR: {
        int r = alloc_reg();
        if (opt_W) reg_owner[r] = node->var->name;
        if (node->var->is_local) {
            if (node->var->ty->kind == TY_VLA || ((node->var->ty->kind == TY_STRUCT || node->var->ty->kind == TY_UNION) && node->var->ty->vla_len_expr)) {
#ifdef ARCH_ARM64
                arm64_load_from_fp_minus(node->var->offset - 8, reg64[r]);
#else
                asm_mov_rbp_reg(cg_sec, r, 8, node->var->offset - 8); // mov -%d(%%rbp), %s
#endif
            } else {
#ifdef ARCH_ARM64
                if (node->var->offset <= 4095)
                    asm_sub_imm(cg_sec, r, 8, FRAME_PTR); // sub %s, %s, #%d
                else {
                    int v = node->var->offset;
                    asm_mov_imm(cg_sec, r, 8, v & 0xffff); // mov %s, #%d
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        asm_movk(cg_sec, r, 1, (uint16_t)(v & 0xffff), s); // movk %s, #%d, lsl #%d
                        v >>= 16;
                        s += 16;
                    }
                    asm_sub_reg_reg(cg_sec, r, r, 8 if 1 == "1" else 4); // sub %s, %s, %s
                }
#else
                asm_lea_rbp_reg(cg_sec, r, 8, node->var->offset); // lea -%d(%%rbp), %s
#endif
            }
        } else {
            if (node->var->is_weak) {
#ifdef __APPLE__
                (void)0 /* .weak symbol */;
#else
                (void)0 /* .weak symbol */;
#endif
            }
#ifdef ARCH_ARM64
            if (node->var->is_weak || var_needs_got(node->var))
                emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
            else
                emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
#else
            if (var_needs_got(node->var))
                (void)0 /* FIXME: mov indirect/mem */;
            else
                asm_lea_rip_reg(cg_sec, r, var_sym_label(node->var)); // mov %s@GOTPCREL(%%rip), %s
#endif
        }
        return r;
    }
    case ND_DEREF:
        return gen(node->lhs);
    case ND_MEMBER: {
        int r = gen_addr(node->lhs);
#ifdef ARCH_ARM64
        if (node->member->offset > 0 && node->member->offset <= 4095)
            asm_add_imm(cg_sec, r, 8, reg64[r]); // add %s, %s, #%d
        else if (node->member->offset > 0) {
            int ti = alloc_reg();
            asm_mov_imm(cg_sec, ti, 8, node->member->offset); // mov %s, #%d
            asm_add_reg_reg(cg_sec, r, r, 8); // add %s, %s, %s
            free_reg(ti);
        } else if (node->member->offset < 0 && -node->member->offset <= 4095)
            asm_sub_imm(cg_sec, r, 8, reg64[r]); // sub %s, %s, #%d
        else if (node->member->offset < 0) {
            int ti = alloc_reg();
            asm_mov_imm(cg_sec, ti, 8, -node->member->offset); // mov %s, #%d
            asm_sub_reg_reg(cg_sec, r, r, 8); // sub %s, %s, %s
            free_reg(ti);
        }
#else
        asm_add_imm(cg_sec, r, 8, node->member->offset); // add $%d, %s
#endif
        return r;
    }
    case ND_COND: {
        // Struct/union ternary lvalue: (cond ? a : b).member
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int cond = gen(node->cond);
#ifdef ARCH_ARM64
        asm_cmp_zero(cg_sec, cond, node->cond->ty->size); // cmp %s, #0
        asm_jcc_label(cg_sec, ARM64_EQ); // b.eq .L.else.%d
        free_reg(cond);
        int then_r = gen_addr(node->then);
        asm_mov_reg_reg(cg_sec, r, then_r, 8); // mov %s, %s
        free_reg(then_r);
        asm_jmp_label(cg_sec); // b .L.end.%d
        (void)0 /* FIXME: label .L.xxx.c */;
        int else_r = gen_addr(node->els);
        asm_mov_reg_reg(cg_sec, r, else_r, 8); // .L.else.%d:
        free_reg(else_r);
        (void)0 /* FIXME: label .L.xxx.c */;
#else
        asm_cmp_zero(cg_sec, cond, node->cond->ty->size); // mov %s, %s
        asm_jcc_label(cg_sec, X86_E); // .L.end.%d:
        free_reg(cond);
        int then_r = gen_addr(node->then);
        asm_mov_reg_reg(cg_sec, then_r, r, 8); // cmp $0, %s
        free_reg(then_r);
        asm_jmp_label(cg_sec); // je .L.else.%d
        (void)0 /* FIXME: label .L.xxx.c */;
        int else_r = gen_addr(node->els);
        asm_mov_reg_reg(cg_sec, else_r, r, 8); // mov %s, %s
        free_reg(else_r);
        (void)0 /* FIXME: label .L.xxx.c */;
#endif
        return r;
    }
    case ND_CAST:
        // Struct/union cast: treat as address of the inner expression
        if (node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION || node->ty->kind == TY_ARRAY)) {
            int r = gen_addr(node->lhs);
            if (r >= 0) return r;
            return -1; // caller should handle non-lvalue
        }
        error_tok(node->tok, "lvalue required as left operand of assignment");
        return -1;
    case ND_COMMA: {
        // Compound literal: evaluate LHS side effects, return address of RHS
        int r1 = gen(node->lhs);
        if (r1 != -1) free_reg(r1);
        return gen_addr(node->rhs);
    }
    case ND_FUNCALL:
        // Struct-returning call used as an lvalue (e.g. passed by address to
        // another function): let gen_funcall allocate a hidden ret buffer and
        // return its address.
        if (node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION))
            return gen_funcall(node, -1);
        error_tok(node->tok, "lvalue required as left operand of assignment");
        return -1;
    case ND_ASSIGN:
        // Assignment expression used as lvalue: return address of lhs
        return gen_addr(node->lhs);
    case ND_STMT_EXPR: {
        // Statement expression used as lvalue (e.g. d = ({ bar(); }))
        // Evaluate the block and return address of the last expression
        int result = -1;
        for (Node *n = node->body; n; n = n->next) {
            int r = gen(n);
            if (node->stmt_expr_result && n->kind == ND_EXPR_STMT && n->lhs == node->stmt_expr_result) {
                result = gen_addr(node->stmt_expr_result);
            }
            if (r != -1) free_reg(r);
        }
        if (result != -1)
            return result;
        error_tok(node->tok, "lvalue required as left operand of assignment");
        return -1;
    }
    case ND_NUM:
    case ND_FNUM:
        return -1; // not an lvalue; caller should handle via temp allocation
    default:
        error_tok(node->tok, "lvalue required as left operand of assignment");
        return -1;
    }
}


static void gen_cond_branch_inv(Node *cond, char *label) {
    if (cond->kind == ND_LOGAND) {
        gen_cond_branch_inv(cond->lhs, label);
        gen_cond_branch_inv(cond->rhs, label);
        return;
    }
    if (cond->kind == ND_LOGOR) {
        int c = ++rcc_label_count;
        char skip_label[32];
        sprintf(skip_label, ".L.or_skip.%d", c);
        // If lhs is true, skip to end (don't branch to label)
        int lhs = gen(cond->lhs);
        asm_cmp_zero(cg_sec, lhs, cond->lhs->ty->size); // fmov d0, %s
        free_reg(lhs);
#ifdef ARCH_ARM64
        size_t o = asm_jcc_label(cg_sec, ARM64_NE); // fmov d1, %s
        asm_fixup_add(cg_sec, o, skip_label, 1); // fcmp d0, d1
#else
        size_t o = asm_jcc_label(cg_sec, X86_NE); // b.ne %s\n  b.vs %s
        asm_fixup_add(cg_sec, o, skip_label, 1); // b.vs .L.fc.skip.%d
#endif
        // If lhs was false, check rhs; if rhs is also false, branch to label
        gen_cond_branch_inv(cond->rhs, label);
        cg_def_label(skip_label); // b.eq %s
        return;
    }
    if (cond->kind == ND_EQ || cond->kind == ND_NE || cond->kind == ND_LT || cond->kind == ND_LE) {
        if (is_flonum(cond->lhs->ty)) {
            int r_lhs = gen(cond->lhs);
            int r_rhs = gen(cond->rhs);
#ifdef ARCH_ARM64
            (void)0 /* FIXME: fmov */;
            (void)0 /* FIXME: fmov */;
            asm_fcmp(cg_sec, 1); // .L.fc.skip.%d:
            if (cond->kind == ND_EQ)
                (void)0 /* FIXME: multi-instruction printf */;
            else if (cond->kind == ND_NE) {
                int c = ++rcc_label_count;
                asm_jcc_label(cg_sec, ARM64_VS); // b.pl %s
                asm_jcc_label(cg_sec, ARM64_EQ); // b.hi %s
                cg_def_label(format(".L.fc.skip.%d", c)); // movq %s, %%xmm0
            } else if (cond->kind == ND_LT)
                asm_jcc_label(cg_sec, ARM64_PL); // movq %s, %%xmm1
            else if (cond->kind == ND_LE)
                asm_jcc_label(cg_sec, ARM64_HI); // ucomisd %%xmm1, %%xmm0
#else
            asm_movq_r_xmm(cg_sec, X86_XMM0, r_lhs); // jne %s\n  jp %s
            asm_movq_r_xmm(cg_sec, X86_XMM1, r_rhs); // jp .L.fc.skip.%d
            asm_ucomisd(cg_sec); // je %s
            if (cond->kind == ND_EQ)
                (void)0 /* FIXME: multi-instruction printf */;
            else if (cond->kind == ND_NE) {
                int c = ++rcc_label_count;
                asm_jcc_label(cg_sec, X86_P); // .L.fc.skip.%d:
                asm_jcc_label(cg_sec, X86_E); // jae %s\n  jp %s
                cg_def_label(format(".L.fc.skip.%d", c)); // ja %s\n  jp %s
            } else if (cond->kind == ND_LT)
                (void)0 /* FIXME: multi-instruction printf */;
            else if (cond->kind == ND_LE)
                (void)0 /* FIXME: multi-instruction printf */;
#endif
            free_reg(r_rhs);
            free_reg(r_lhs);
            return;
        }
        int r_lhs = gen(cond->lhs);
        int sz = op_size(cond->lhs->ty);
        if (sz < op_size(cond->rhs->ty))
            sz = op_size(cond->rhs->ty);
        if (cond->rhs->kind == ND_NUM && cond->rhs->val == (int32_t)cond->rhs->val) {
#ifdef ARCH_ARM64
            int32_t imm = (int32_t)cond->rhs->val;
            if (imm >= 0 && imm <= 4095) {
                asm_cmp_imm(cg_sec, r_lhs, sz, imm); // cmp %s, #%d
            } else if (imm < 0 && imm >= -4095) {
                secbuf_emit32le(cg_sec, arm64_subs_imm(sz == 8 ? 1 : 0, 31, CG_ARM_REG(r_lhs), -imm, 0)); // cmn %s, #%d
            } else {
                emit_mov_imm64("x16", (uint64_t)(int64_t)imm);
                asm_cmp_reg_reg(cg_sec, r_lhs, 16, sz); // cmp %s, %s\n", reg(r_lhs, sz), sz <= 4 ? "w16" : "x16
            }
#else
            asm_cmp_imm(cg_sec, r_lhs, sz, (int32_t)cond->rhs->val); // cmp $%d, %s
#endif
        } else {
            int r_rhs = gen(cond->rhs);
#ifdef ARCH_ARM64
            asm_cmp_reg_reg(cg_sec, r_lhs, r_rhs, 8); // cmp %s, %s
#else
            asm_cmp_reg_reg(cg_sec, r_rhs, r_lhs, 8); // cmp %s, %s
#endif
            free_reg(r_rhs);
        }
        free_reg(r_lhs);

#ifdef ARCH_ARM64
        const char *jmp = "b";
        if (cond->kind == ND_EQ) jmp = "b.ne";
        else if (cond->kind == ND_NE)
            jmp = "b.eq";
        else if (cond->kind == ND_LT)
            jmp = use_unsigned_cmp(cond) ? "b.hs" : "b.ge";
        else if (cond->kind == ND_LE)
            jmp = use_unsigned_cmp(cond) ? "b.hi" : "b.gt";
#else
        char *jmp = "";
        if (cond->kind == ND_EQ) jmp = "jne";
        else if (cond->kind == ND_NE)
            jmp = "je";
        else if (cond->kind == ND_LT)
            jmp = use_unsigned_cmp(cond) ? "jae" : "jge";
        else if (cond->kind == ND_LE)
            jmp = use_unsigned_cmp(cond) ? "ja" : "jg";
#endif

        // Emit conditional branch
#ifdef ARCH_ARM64
        if (cond->kind == ND_EQ) {
            size_t o = asm_jcc_label(cg_sec, ARM64_NE); // %s %s
            asm_fixup_add(cg_sec, o, label, 1); // cmp %s, #0
        } else if (cond->kind == ND_NE) {
            size_t o = asm_jcc_label(cg_sec, ARM64_EQ); // b.eq %s
            asm_fixup_add(cg_sec, o, label, 1); // cmp $0, %s
        } else if (cond->kind == ND_LT) {
            size_t o = asm_jcc_label(cg_sec, use_unsigned_cmp(cond) ? ARM64_HS : ARM64_GE); // je %s
            asm_fixup_add(cg_sec, o, label, 1); // %s %s
        } else if (cond->kind == ND_LE) {
            size_t o = asm_jcc_label(cg_sec, use_unsigned_cmp(cond) ? ARM64_HI : ARM64_GT); // cmp %s, #0
            asm_fixup_add(cg_sec, o, label, 1); // b.eq %s
        }
#else
        if (cond->kind == ND_EQ) {
            size_t o = asm_jcc_label(cg_sec, X86_NE); // cmp $0, %s
            asm_fixup_add(cg_sec, o, label, 1); // je %s
        } else if (cond->kind == ND_NE) {
            size_t o = asm_jcc_label(cg_sec, X86_E);
            asm_fixup_add(cg_sec, o, label, 1);
        } else if (cond->kind == ND_LT) {
            size_t o = asm_jcc_label(cg_sec, use_unsigned_cmp(cond) ? X86_AE : X86_GE);
            asm_fixup_add(cg_sec, o, label, 1);
        } else if (cond->kind == ND_LE) {
            size_t o = asm_jcc_label(cg_sec, use_unsigned_cmp(cond) ? X86_A : X86_G);
            asm_fixup_add(cg_sec, o, label, 1);
        }
#endif
        return;
    }

    int r = gen(cond);
#ifdef ARCH_ARM64
    asm_cmp_zero(cg_sec, r, cond->ty->size);
    free_reg(r);
    size_t cond_off = asm_jcc_label(cg_sec, ARM64_EQ);
    asm_fixup_add(cg_sec, cond_off, label, 1);
#else
    asm_cmp_zero(cg_sec, r, cond->ty->size);
    free_reg(r);
    size_t cond_off = asm_jcc_label(cg_sec, X86_E);
    asm_fixup_add(cg_sec, cond_off, label, 1);
#endif
}

// Generate code for a given node.
static int gen(Node *node) {
    if (!node) return -1;

    if (opt_g)
        emit_loc(node);

    switch (node->kind) {
    case ND_NUM: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        uint64_t v = (uint64_t)(long long)node->val;
        // Use x register for 64-bit immediates, w for 32-bit (stops at lsl #16)
        if (op_size(node->ty) == 4) {
            asm_mov_imm(cg_sec, r, 4, v & 0xffff); // mov %s, #%llu
            v >>= 16;
            if (v) {
                asm_movk(cg_sec, r, 0, (uint16_t)(v), 16); // movk %s, #%llu, lsl #16
            }
        } else {
            asm_mov_imm(cg_sec, r, 8, v & 0xffff); // mov %s, #%llu
            v >>= 16;
            int shift = 16;
            while (v) {
                asm_movk(cg_sec, r, 1, (uint16_t)(v & 0xffff), shift); // movk %s, #%llu, lsl #%d
                v >>= 16;
                shift += 16;
            }
        }
#else
        if (node->val == (int32_t)node->val) {
            asm_mov_imm(cg_sec, r, op_size(node->ty), (long long)node->val); // mov $%lld, %s
        } else {
            asm_mov_imm(cg_sec, r, 8, (long long)node->val); // movabs $%lld, %s
        }
#endif
        return r;
    }
    case ND_FNUM: {
        int r = alloc_reg();
        int id = add_float_literal(node->fval, 8); // Always store as double for computations
#ifdef ARCH_ARM64
        emit_adrp_add(reg64[r], format(".LF%d", id));
        asm_ldr_fp(cg_sec, 0, r, 8); // ldr d0, [%s]
        asm_fmov_f2i(cg_sec, r, 0, 1); // fmov %s, d0
#else
        (void)0 /* FIXME: rip-relative */;
        (void)0 /* TODO: movq to/from xmm */;
#endif
        return r;
    }
    case ND_LVAR: {
        int r = alloc_reg();
        if (opt_W) reg_owner[r] = node->var->name;
#ifndef ARCH_ARM64
        char *label = var_label(node->var);
#endif
        if (node->var->ty->kind == TY_VLA || ((node->var->ty->kind == TY_STRUCT || node->var->ty->kind == TY_UNION) && node->var->ty->vla_len_expr)) {
            if (node->var->is_local) {
#ifdef ARCH_ARM64
                arm64_load_from_fp_minus(node->var->offset - 8, reg64[r]);
#else
                asm_mov_rbp_reg(cg_sec, r, 8, node->var->offset - 8); // mov -%d(%%rbp), %s
#endif
            } else {
#ifdef ARCH_ARM64
                if (var_needs_got(node->var))
                    emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
                else
                    emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
                asm_ldr_reg_off(cg_sec, r, r, 8, 0); // ldr %s, [%s]
#else
                if (var_needs_got(node->var)) {
                    (void)0 /* FIXME: mov indirect/mem */;
                    (void)0 /* FIXME: indirect mov */;
                } else {
                    asm_lea_rip_reg(cg_sec, r, var_sym_label(node->var)); // mov %s@GOTPCREL(%%rip), %s
                    X86Mem m = {CG_X86_REG(r), X86_NOREG, 1, 0};
                    x86_mov_rm(cg_sec, 8, CG_X86_REG(r), m); // mov (%s), %s
                }
#endif
            }
        } else if (node->var->ty->kind == TY_ARRAY) {
            if (node->var->is_local)
#ifdef ARCH_ARM64
                if (node->var->offset <= 4095)
                    asm_sub_imm(cg_sec, r, 8, FRAME_PTR); // sub %s, %s, #%d
                else {
                    int v = node->var->offset;
                    asm_mov_imm(cg_sec, r, 8, v & 0xffff); // mov %s, #%d
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        asm_movk(cg_sec, r, 1, (uint16_t)(v & 0xffff), s); // movk %s, #%d, lsl #%d
                        v >>= 16;
                        s += 16;
                    }
                    asm_sub_reg_reg(cg_sec, r, r, 8 if 1 == "1" else 4); // sub %s, %s, %s
                }
#else
                asm_lea_rbp_reg(cg_sec, r, 8, node->var->offset); // lea -%d(%%rbp), %s
#endif
            else
#ifdef ARCH_ARM64
                if (var_needs_got(node->var))
                emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
            else
                emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
#else
                if (var_needs_got(node->var))
                (void)0 /* FIXME: mov indirect/mem */;
            else
                asm_lea_rip_reg(cg_sec, r, var_sym_label(node->var)); // mov %s@GOTPCREL(%%rip), %s
#endif
        } else if (!node->var->is_local && node->var->is_function) {
            if (node->var->is_weak) {
#ifdef __APPLE__
                (void)0 /* .weak symbol */;
#else
                (void)0 /* .weak symbol */;
#endif
            }
#ifdef ARCH_ARM64
            if (node->var->is_weak || var_needs_got(node->var))
                emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
            else
                emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
#else
            if (var_needs_got(node->var))
                (void)0 /* FIXME: mov indirect/mem */;
            else
                asm_lea_rip_reg(cg_sec, r, var_sym_label(node->var)); // mov %s@GOTPCREL(%%rip), %s
#endif
        } else if (is_flonum(node->var->ty)) {
            {
                if (node->var->is_local) {
                    if (node->var->ty->size == 4) {
#ifdef ARCH_ARM64
                        arm64_load_from_fp_minus(node->var->offset, "s0");
                        (void)0 /* arm64 fcvt d0,s0 */;
#else
                        (void)0 /* FIXME: float op */;
                        asm_cvtss2sd(cg_sec); // fcvt d0, s0
#endif
                    } else {
#ifdef ARCH_ARM64
                        arm64_load_from_fp_minus(node->var->offset, "d0");
#else
                        (void)0 /* FIXME: float op */;
#endif
                    }
                } else {
                    if (node->var->ty->size == 4) {
#ifdef ARCH_ARM64
                        if (var_needs_got(node->var))
                            emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
                        else
                            emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
                        asm_ldr_fp(cg_sec, 0, r, 4); // ldr s0, [%s]
                        asm_fcvt(cg_sec, 1, 0, 0, 0); // fcvt d0, s0
#else
                        if (var_needs_got(node->var)) {
                            (void)0 /* FIXME: mov indirect/mem */;
                            (void)0 /* FIXME: float op */;
                        } else {
                            (void)0 /* FIXME: rip-relative */;
                        }
                        asm_cvtss2sd(cg_sec); // mov %s@GOTPCREL(%%rip), %s
#endif
                    } else {
#ifdef ARCH_ARM64
                        if (var_needs_got(node->var))
                            emit_adrp_got(reg64[r], asm_sym_name(var_sym_label(node->var)));
                        else
                            emit_adrp_add(reg64[r], asm_sym_name(var_sym_label(node->var)));
                        asm_ldr_fp(cg_sec, 0, r, 8); // ldr d0, [%s]
#else
                        if (var_needs_got(node->var)) {
                            (void)0 /* FIXME: mov indirect/mem */;
                            (void)0 /* FIXME: float op */;
                        } else {
                            (void)0 /* FIXME: rip-relative */;
                        }
#endif
                    }
                }
#ifdef ARCH_ARM64
                asm_fmov_f2i(cg_sec, r, 0, 1); // mov %s@GOTPCREL(%%rip), %s
#else
                asm_movq_xmm_r(cg_sec, r, X86_XMM0); // movsd (%s), %%xmm0
#endif
            }
        } else {
            if (node->var->is_local)
#ifdef ARCH_ARM64
                emit_load(node->ty, r, ARM64_BASE_FP, -node->var->offset);
#else
                emit_load(node->ty, r, X86_BASE_RBP, node->var->offset);
#endif
            else {
#ifdef ARCH_ARM64
                // Global variable: load address via ADRP+ADD, then deref
                int ta = alloc_reg();
                if (var_needs_got(node->var))
                    emit_adrp_got(reg64[ta], asm_sym_name(var_sym_label(node->var)));
                else
                    emit_adrp_add(reg64[ta], asm_sym_name(var_sym_label(node->var)));
                emit_load(node->ty, r, ta, 0);
                free_reg(ta);
#else
                if (var_needs_got(node->var)) {
                    (void)0 /* FIXME: mov indirect/mem */;
                    emit_load(node->ty, r, r, 0);
                } else {
                    asm_lea_rip_reg(cg_sec, r, var_sym_label(node->var)); // mov %s@GOTPCREL(%%rip), %s
                    emit_load(node->ty, r, r, 0);
                }
#endif
            }
        }
        return r;
    }
    case ND_ASSIGN: {
        if (node->lhs->ty->kind == TY_ARRAY || node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION) {
            int c = ++rcc_label_count;
            // For VLA-containing structs, the "address" is the VLA data pointer (gen()),
            // not the VLA descriptor address (gen_addr()).
            bool lhs_vla_struct = (node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION) && node->lhs->ty->vla_len_expr;
            int dst = lhs_vla_struct ? gen(node->lhs) : gen_addr(node->lhs);
            int src;
            if (node->rhs->kind == ND_FUNCALL && node->rhs->ty &&
                (node->rhs->ty->kind == TY_STRUCT || node->rhs->ty->kind == TY_UNION)) {
                gen_funcall(node->rhs, dst);
                return dst;
            }
            // String literal → char array: limit copy to actual string size, zero-fill rest
            if (node->rhs->kind == ND_STR && node->lhs->ty->kind == TY_ARRAY) {
                src = gen(node->rhs); // loads address of string
                int str_len = 0;
                for (StrLit *s = all_strs; s; s = s->next) {
                    if (s->id != node->rhs->str_id) continue;
                    if (s->prefix != 0) {
                        // Wide string: count Unicode chars and multiply by elem_size
                        int count = 0;
                        char *p = s->str;
                        while (*p) {
                            char *next;
                            decode_utf8(&next, p);
                            if (next == p) {
                                p++;
                                continue;
                            }
                            p = next;
                            count++;
                        }
                        str_len = (count + 1) * s->elem_size;
                    } else {
                        str_len = s->len + s->elem_size;
                    }
                    break;
                }
                int lhs_size = node->lhs->ty->size;
                int copy_len = str_len < lhs_size ? str_len : lhs_size;
                if (copy_len > 0) {
#ifdef ARCH_ARM64
                    asm_mov_imm(cg_sec, 9, 8, copy_len); // mov x9, #%d
                    cg_def_label(format(".L.strcpy.%d", c)); // .L.copy.%d:
                    asm_cmp_zero(cg_sec, 9, 8); // cmp x9, #0
                    asm_jcc_label(cg_sec, ARM64_EQ); // b.eq .L.copy_end.%d
                    asm_dec(cg_sec, 9, 8); // sub x9, x9, #1
                    (void)0 /* FIXME: sized ld/st */;
                    (void)0 /* FIXME: sized ld/st */;
                    asm_jmp_label(cg_sec); // ldrb w16, [%s, x9]
                    cg_def_label(format(".L.strcpy_end.%d", c)); // strb w16, [%s, x9]
                }
                if (copy_len < lhs_size) {
                    // Zero dst[copy_len .. lhs_size-1]; count x12 from lhs_size down to copy_len
                    int c2 = ++rcc_label_count;
                    asm_mov_imm(cg_sec, 9, 8, lhs_size); // b .L.copy.%d
                    cg_def_label(format(".L.strzero.%d", c2)); // .L.copy_end.%d:
                    if (copy_len >= 0 && copy_len <= 4095)
                        asm_cmp_imm(cg_sec, 9, 8, copy_len); // mov x9, #%d
                    else {
                        asm_mov_imm(cg_sec, 16, 8, copy_len & 0xffff); // .L.copy.%d:
                        if (copy_len >> 16)
                            asm_movk(cg_sec, 16, 1, (uint16_t)(copy_len >> 16), 16); // cmp x9, #%d
                        asm_cmp_reg_reg(cg_sec, 9, 16, 8 if 1 == "1" else 4); // mov x16, #%d
                    }
                    asm_jcc_label(cg_sec, ARM64_EQ); // movk x16, #%d, lsl #16
                    asm_dec(cg_sec, 9, 8); // cmp x9, x16
                    (void)0 /* FIXME: sized ld/st */;
                    asm_jmp_label(cg_sec); // b.eq .L.copy_end.%d
                    cg_def_label(format(".L.strzero_end.%d", c2)); // sub x9, x9, #1
                }
#else
                    asm_mov_imm(cg_sec, 1, 8, copy_len); // strb wzr, [%s, x9]
                    cg_def_label(format(".L.strcpy.%d", c)); // b .L.copy.%d
                    asm_cmp_zero(cg_sec, 1, 8); // .L.copy_end.%d:
                    asm_jcc_label(cg_sec, X86_E); // movq $%d, %%rcx
                    (void)0 /* FIXME: sized mov */;
                    (void)0 /* FIXME: sized mov */;
                    asm_dec(cg_sec, 1, 8); // .L.copy.%d:
                    asm_jmp_label(cg_sec); // cmpq $0, %%rcx
                    cg_def_label(format(".L.strcpy_end.%d", c)); // je .L.copy_end.%d
                }
                if (copy_len < lhs_size) {
                    asm_mov_imm(cg_sec, 1, 8, lhs_size - copy_len); // movb -1(%s,%%rcx), %%al
                    int c2 = ++rcc_label_count;
                    cg_def_label(format(".L.strzero.%d", c2)); // movb %%al, -1(%s,%%rcx)
                    asm_cmp_zero(cg_sec, 1, 8); // subq $1, %%rcx
                    asm_jcc_label(cg_sec, X86_E); // jmp .L.copy.%d
                    (void)0 /* FIXME: sized mov */;
                    asm_dec(cg_sec, 1, 8); // .L.copy_end.%d:
                    asm_jmp_label(cg_sec); // movq $%d, %%rcx
                    cg_def_label(format(".L.strzero_end.%d", c2)); // .L.copy.%d:
                }
#endif
                free_reg(src);
                return dst;
            }

            if (node->rhs->ty && (node->rhs->ty->kind == TY_STRUCT || node->rhs->ty->kind == TY_UNION || node->rhs->ty->kind == TY_ARRAY))
                src = gen_addr(node->rhs);
            else
                src = gen(node->rhs);

            // If RHS is a scalar (e.g. pointer/function) and LHS is a small struct/union,
            // store the value directly instead of copying bytes from the address in the register.
            if (node->rhs->ty && !(node->rhs->ty->kind == TY_STRUCT || node->rhs->ty->kind == TY_UNION || node->rhs->ty->kind == TY_ARRAY) && node->lhs->ty->size <= 8) {
#ifdef ARCH_ARM64
                emit_store(node->lhs->ty, src, dst, 0);
#else
                {
                    int st_sz = node->lhs->ty->size;
                    if (st_sz < 4) st_sz = st_sz;
                    X86Mem m = {CG_X86_REG(dst), X86_NOREG, 1, 0};
                    x86_mov_mr(cg_sec, st_sz, m, CG_X86_REG(src)); // mov %s, (%s)
                }
#endif
                free_reg(src);
                return dst;
            }

            // For VLA-containing structs, use the runtime size instead of ty->size
            bool copy_is_vla_struct = (node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION) && node->lhs->ty->vla_len_expr;
            int r_vla_sz = -1;
            if (copy_is_vla_struct)
                r_vla_sz = gen(node->lhs->ty->vla_len_expr);
#ifdef ARCH_ARM64
            if (copy_is_vla_struct && r_vla_sz >= 0)
                (void)0 /* FIXME: mov phy */;
            else
                asm_mov_imm(cg_sec, 9, 8, node->lhs->ty->size); // mov x9, %s
            (void)0 /* FIXME: label .L.xxx.c */;
            asm_cmp_zero(cg_sec, 9, 8); // mov x9, #%d
            asm_jcc_label(cg_sec, ARM64_EQ); // .L.copy.%d:
            asm_dec(cg_sec, 9, 8); // cmp x9, #0
            (void)0 /* FIXME: sized ld/st */;
            (void)0 /* FIXME: sized ld/st */;
            asm_jmp_label(cg_sec); // b.eq .L.copy_end.%d
            (void)0 /* FIXME: label .L.xxx.c */;
#else
            if (copy_is_vla_struct && r_vla_sz >= 0)
                asm_mov_reg_reg(cg_sec, 1, r_vla_sz, 8); // sub x9, x9, #1
            else
                asm_mov_imm(cg_sec, 1, 8, node->lhs->ty->size); // ldrb w16, [%s, x9]
            cg_def_label(format(".L.copy.%d", c)); // strb w16, [%s, x9]
            asm_cmp_zero(cg_sec, 1, 8); // b .L.copy.%d
            size_t cj1 = asm_jcc_label(cg_sec, X86_E); // .L.copy_end.%d:
            asm_fixup_add(cg_sec, cj1, format(".L.copy_end.%d", c), 1); // movq %s, %%rcx
            {
                X86Mem msrc = x86_mem_idx(CG_X86_REG(src), CG_X86_REG(1), 1, -1); // movq $%d, %%rcx
                X86Mem mdst = x86_mem_idx(CG_X86_REG(dst), CG_X86_REG(1), 1, -1); // .L.copy.%d:
                x86_mov_rm(cg_sec, 1, X86_RAX, msrc); // cmpq $0, %%rcx
                x86_mov_mr(cg_sec, 1, mdst, X86_RAX); // je .L.copy_end.%d
            }
            asm_dec(cg_sec, 1, 8); // movb -1(%s,%%rcx), %%al
            size_t cj2 = asm_jmp_label(cg_sec); // movb %%al, -1(%s,%%rcx)
            asm_fixup_add(cg_sec, cj2, format(".L.copy.%d", c), 0); // subq $1, %%rcx
            cg_def_label(format(".L.copy_end.%d", c)); // jmp .L.copy.%d
#endif
            if (r_vla_sz >= 0) free_reg(r_vla_sz);
            free_reg(src);
            return dst;
        }
        if (is_flonum(node->lhs->ty)) {
#ifdef ARCH_ARM64
            int r2 = gen(node->rhs);
            int r1 = gen_addr(node->lhs);
            (void)0 /* FIXME: fmov */;
            if (node->lhs->ty->size == 4) {
                asm_fcvt(cg_sec, 3, 0, 0, 0); // fmov d0, %s
                asm_str_fp(cg_sec, 0, r1, 4); // fcvt s0, d0
            } else {
                asm_str_fp(cg_sec, 0, r1, 8); // str s0, [%s]
            }
            free_reg(r1);
            return r2;
#else
#ifndef _WIN32
            if (node->lhs->ty->kind == TY_LDOUBLE) {
                // Store long double as 64-bit double (truncated)
                int r2 = gen(node->rhs);
                int r1 = gen_addr(node->lhs);
                (void)0 /* TODO: movq to/from xmm */;
                (void)0 /* FIXME: float op */;
                free_reg(r2);
                free_reg(r1);
                int dummy = alloc_reg();
                asm_movq_zero(cg_sec, dummy); // movq %s, %%xmm0
                return dummy;
            }
#endif
            int r2 = gen(node->rhs);
            int r1 = gen_addr(node->lhs);
            (void)0 /* TODO: movq to/from xmm */;
            if (node->lhs->ty->size == 4) {
                asm_cvtsd2ss(cg_sec); // movsd %%xmm0, (%s)
                (void)0 /* FIXME: float op */;
            } else {
                (void)0 /* FIXME: float op */;
            }
            free_reg(r1);
            return r2;
#endif
        }
        if (node->lhs->kind == ND_LVAR && node->lhs->var->is_local && node->lhs->var->ty->kind != TY_ARRAY) {
            int r2 = gen(node->rhs);
#ifdef ARCH_ARM64
            emit_store_offset(node->lhs->ty, r2, FRAME_PTR, -node->lhs->var->offset);
#else
            asm_mov_reg_rbp(cg_sec, r2, node->lhs->ty->size, node->lhs->var->offset); // mov %s, -%d(%%rbp)
#endif
            // Truncate result to match the variable's type width for unsigned narrow types
            if (node->lhs->ty->is_unsigned && node->lhs->ty->size < 4) {
                int mask = (1 << (node->lhs->ty->size * 8)) - 1;
#ifdef ARCH_ARM64
                (void)0 /* FIXME: and variant */;
#else
                (void)0 /* FIXME: and variant */;
#endif
            }
            return r2;
        }
        // Bitfield assignment: read-modify-write
        if (node->lhs->kind == ND_MEMBER && node->lhs->member &&
            node->lhs->member->bit_width > 0) {
            int bw = node->lhs->member->bit_width;
            int bo = node->lhs->member->bit_offset;
            int unit_sz = node->lhs->member->bf_load_size
                ? node->lhs->member->bf_load_size
                : node->lhs->member->ty->size;
            unsigned long long mask = ((1ULL << bw) - 1) << bo;

            // Check if RHS reads the same bitfield (compound assignment like s.x += 1)
            bool rhs_reads_same = false;
            if (node->rhs->kind == ND_MEMBER && node->rhs->member == node->lhs->member) {
                rhs_reads_same = true;
            } else if (node->rhs->kind == ND_ADD || node->rhs->kind == ND_SUB ||
                       node->rhs->kind == ND_MUL || node->rhs->kind == ND_DIV ||
                       node->rhs->kind == ND_BITAND || node->rhs->kind == ND_BITOR ||
                       node->rhs->kind == ND_BITXOR) {
                if (node->rhs->lhs && node->rhs->lhs->kind == ND_MEMBER &&
                    node->rhs->lhs->member == node->lhs->member)
                    rhs_reads_same = true;
                if (node->rhs->rhs && node->rhs->rhs->kind == ND_MEMBER &&
                    node->rhs->rhs->member == node->lhs->member)
                    rhs_reads_same = true;
            }

            // Helper: emit unsigned load of unit_sz bytes
            // Helper: emit unsigned load of unit_sz bytes
#ifdef ARCH_ARM64
#define BF_LOAD(sz, ra, rt) do { asm_ldr_reg_off(cg_sec, rt, ra, sz, 0); } while (0)
#define BF_STORE(sz, ra, rt) do { asm_str_reg_off(cg_sec, rt, ra, sz, 0); } while (0)
#else
#define BF_LOAD(sz, ra, rt) do { \
    if ((sz) == 1) asm_movzx_mem_reg(cg_sec, rt, ra, 4, 1); \
    else if ((sz) == 2) asm_movzx_mem_reg(cg_sec, rt, ra, 4, 2); \
    else if ((sz) == 4) asm_mov_mem_reg(cg_sec, rt, ra, 4); \
    else asm_mov_mem_reg(cg_sec, rt, ra, 8); \
} while (0)
#define BF_STORE(sz, ra, rt) do { \
    if ((sz) == 1) asm_mov_reg_mem(cg_sec, rt, ra, 1); \
    else if ((sz) == 2) asm_mov_reg_mem(cg_sec, rt, ra, 2); \
    else if ((sz) == 4) asm_mov_reg_mem(cg_sec, rt, ra, 4); \
    else asm_mov_reg_mem(cg_sec, rt, ra, 8); \
} while (0)
#endif

            // Generate RHS (the new value to assign)
            int r2 = gen(node->rhs);

            if (rhs_reads_same) {
                int ra = gen_addr(node->lhs);
                int rt = alloc_reg();
                int eff_sz_rhs = unit_sz > 8 ? 8 : unit_sz;
                BF_LOAD(eff_sz_rhs, ra, rt);
#ifdef ARCH_ARM64
                emit_mov_imm64("x16", ~mask);
                asm_and_reg_reg(cg_sec, rt, 16, 8); // and %s, %s, x16
                emit_mov_imm64("x16", (1ULL << bw) - 1);
                int rv = alloc_reg();
                asm_mov_reg_reg(cg_sec, rv, r2, 8); // mov %s, %s
                asm_and_reg_reg(cg_sec, rv, 16, 8); // and %s, %s, x16
                if (bo > 0) asm_shl_imm(cg_sec, rv, 8, (uint8_t)(reg64[rv])); // lsl %s, %s, #%d
                asm_or_reg_reg(cg_sec, rt, rt, 8); // orr %s, %s, %s
                BF_STORE(eff_sz_rhs, ra, rt);
                free_reg(rv);
                // Reload stored bitfield value for assignment expression result
                {
                    int new_eff_sz_rhs = eff_sz_rhs;
                    if (new_eff_sz_rhs <= 2)
                        (void)0 /* FIXME: sized ld/st */;
                    else if (new_eff_sz_rhs == 4)
                        asm_ldr_reg_off(cg_sec, rt, ra, 4, 0); // ldrh %s, [%s]
                    else
                        asm_ldr_reg_off(cg_sec, rt, ra, 8, 0); // ldr %s, [%s]
                    if (bo > 0)
                        asm_shr_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // ldr %s, [%s]
                    if (bw < new_eff_sz_rhs * 8) {
                        if (node->lhs->member->ty->is_unsigned || node->lhs->member->ty->is_enum) {
                            emit_mov_imm64("x16", (1ULL << bw) - 1);
                            asm_and_reg_reg(cg_sec, rt, 16, 8); // lsr %s, %s, #%d
                        } else {
                            int shift = 64 - bw;
                            asm_shl_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // and %s, %s, x16
                            asm_sar_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // lsl %s, %s, #%d
                        }
                    }
                    free_reg(r2);
                    int ret_reg = rt;
                    free_reg(ra);
                    return ret_reg;
                }
            }

            // Simple assignment: read-modify-write
            free_reg(gen_addr(node->lhs));
            int ra = gen_addr(node->lhs);
            int rt = alloc_reg();
            int eff_sz = unit_sz > 8 ? 8 : unit_sz;
            BF_LOAD(eff_sz, ra, rt);
            emit_mov_imm64("x16", ~mask);
            asm_and_reg_reg(cg_sec, rt, 16, 8); // and %s, %s, x16
            emit_mov_imm64("x16", (1ULL << bw) - 1);
            int rv = alloc_reg();
            asm_mov_reg_reg(cg_sec, rv, r2, 8); // mov %s, %s
            asm_and_reg_reg(cg_sec, rv, 16, 8); // and %s, %s, x16
            if (bo > 0) asm_shl_imm(cg_sec, rv, 8, (uint8_t)(reg64[rv])); // lsl %s, %s, #%d
            asm_or_reg_reg(cg_sec, rt, rt, 8); // orr %s, %s, %s
            BF_STORE(eff_sz, ra, rt);
            if (unit_sz > 8 && bo + bw > 64) {
                int overflow = bo + bw - 64;
                unsigned int ovf_mask = (1u << overflow) - 1;
                asm_add_imm(cg_sec, ra, 8, 8); // add %s, %s, #8
                (void)0 /* FIXME: sized ld/st */;
                (void)0 /* FIXME: and variant */;
                asm_mov_reg_reg(cg_sec, rv, r2, 8); // ldrb %s, [%s]
                asm_shr_imm(cg_sec, rv, 8, (uint8_t)(reg64[rv])); // and %s, %s, #%u
                (void)0 /* FIXME: and variant */;
                asm_or_reg_reg(cg_sec, rt, rt, 4); // mov %s, %s
                (void)0 /* FIXME: sized ld/st */;
            }
#else
                if (bo > 0) asm_shr_imm(cg_sec, rt, 8, (uint8_t)(bo)); // lsr %s, %s, #%d
                if (bw < unit_sz * 8) {
                    unsigned long long m = (1ULL << bw) - 1;
                    asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(m)); // and %s, %s, #%u
                    asm_and_reg_reg(cg_sec, rt, 0, 8); // orr %s, %s, %s
                }
                // Re-load for modify
                BF_LOAD(unit_sz, ra, rt);
                asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(~mask)); // strb %s, [%s]
                asm_and_reg_reg(cg_sec, rt, 0, 8); // shr $%d, %s
                int rv = alloc_reg();
                asm_mov_reg_reg(cg_sec, r2, rv, 8); // movabsq $%llu, %%rax
                asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)((1ULL << bw) - 1)); // andq %%rax, %s
                asm_and_reg_reg(cg_sec, rv, 0, 8); // movabsq $%llu, %%rax
                if (bo > 0) asm_shl_imm(cg_sec, rv, 8, (uint8_t)(bo)); // andq %%rax, %s
                asm_or_reg_reg(cg_sec, rv, rt, 8); // mov %s, %s
                BF_STORE(unit_sz, ra, rt);
                free_reg(rv);
                // Reload stored bitfield value for assignment expression result
                {
                    int new_eff_sz = unit_sz > 8 ? 8 : unit_sz;
                    if (new_eff_sz == 1)
                        (void)0 /* FIXME: indirect ext */;
                    else if (new_eff_sz == 2)
                        (void)0 /* FIXME: indirect ext */;
                    else if (new_eff_sz == 4)
                        (void)0 /* FIXME: indirect mov */;
                    else
                        (void)0 /* FIXME: indirect mov */;
                    if (bo > 0)
                        asm_shr_imm(cg_sec, rt, 8, (uint8_t)(bo)); // movabsq $%llu, %%rax
                    if (bw < new_eff_sz * 8) {
                        if (node->lhs->member->ty->is_unsigned || node->lhs->member->ty->is_enum) {
                            unsigned long long m = (1ULL << bw) - 1;
                            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(m)); // andq %%rax, %s
                            asm_and_reg_reg(cg_sec, rt, 0, 8); // shl $%d, %s
                        } else {
                            int shift = 64 - bw;
                            asm_shl_imm(cg_sec, rt, 8, (uint8_t)(shift)); // or %s, %s
                            asm_sar_imm(cg_sec, rt, 8, (uint8_t)(shift)); // movzbl (%s), %s
                        }
                    }
                    free_reg(r2);
                    int ret_reg = rt;
                    free_reg(ra);
                    return ret_reg;
                }
            }

            // Simple assignment: read-modify-write
            free_reg(gen_addr(node->lhs)); // discard; re-gen below
            int ra = gen_addr(node->lhs);
            int rt = alloc_reg();
            int eff_sz = unit_sz > 8 ? 8 : unit_sz;
            BF_LOAD(eff_sz, ra, rt);
            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(~mask)); // movabsq $%llu, %%rax
            asm_and_reg_reg(cg_sec, rt, 0, 8); // andq %%rax, %s
            int rv = alloc_reg();
            asm_mov_reg_reg(cg_sec, r2, rv, 8); // mov %s, %s
            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)((1ULL << bw) - 1)); // movabsq $%llu, %%rax
            asm_and_reg_reg(cg_sec, rv, 0, 8); // andq %%rax, %s
            if (bo > 0) asm_shl_imm(cg_sec, rv, 8, (uint8_t)(bo)); // shl $%d, %s
            asm_or_reg_reg(cg_sec, rv, rt, 8); // or %s, %s
            BF_STORE(eff_sz, ra, rt);
            // Handle overflow bits beyond the first 8 bytes
            if (unit_sz > 8 && bo + bw > 64) {
                int overflow = bo + bw - 64;
                unsigned int ovf_mask = (1u << overflow) - 1;
                x86_add_ri(cg_sec, 8, CG_X86_REG(ra), 8); // add $8, %s
                (void)0 /* FIXME: indirect ext */;
                (void)0 /* FIXME: and variant */;
                asm_mov_reg_reg(cg_sec, r2, rv, 8); // movzbl (%s), %s
                asm_shr_imm(cg_sec, rv, 8, (uint8_t)(64 - bo)); // and $%u, %s
                (void)0 /* FIXME: and variant */;
                asm_or_reg_reg(cg_sec, rv, rt, 4); // mov %s, %s
                (void)0 /* FIXME: sized mov */;
            }
#endif
#undef BF_LOAD
#undef BF_STORE
            free_reg(rv);
#ifdef ARCH_ARM64
            // ARM64 reload for assignment expression result
            {
                int new_eff_sz = eff_sz;
                if (new_eff_sz <= 2)
                    (void)0 /* FIXME: sized ld/st */;
                else if (new_eff_sz == 4)
                    asm_ldr_reg_off(cg_sec, rt, ra, 4, 0); // ldrh %s, [%s]
                else
                    asm_ldr_reg_off(cg_sec, rt, ra, 8, 0); // ldr %s, [%s]
                if (bo > 0)
                    asm_shr_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // ldr %s, [%s]
                if (bw < new_eff_sz * 8) {
                    if (node->lhs->member && (node->lhs->member->ty->is_unsigned || node->lhs->member->ty->is_enum)) {
                        emit_mov_imm64("x16", (1ULL << bw) - 1);
                        asm_and_reg_reg(cg_sec, rt, 16, 8); // lsr %s, %s, #%d
                    } else {
                        int shift = 64 - bw;
                        asm_shl_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // and %s, %s, x16
                        asm_sar_imm(cg_sec, rt, 8, (uint8_t)(reg64[rt])); // lsl %s, %s, #%d
                    }
                }
                free_reg(r2);
                int ret_reg = rt;
                free_reg(ra);
                return ret_reg;
            }
#else
            // x86_64 reload for assignment expression result
            {
                int new_eff_sz = eff_sz;
                if (new_eff_sz == 1)
                    (void)0 /* FIXME: indirect ext */;
                else if (new_eff_sz == 2)
                    (void)0 /* FIXME: indirect ext */;
                else if (new_eff_sz == 4)
                    (void)0 /* FIXME: indirect mov */;
                else
                    (void)0 /* FIXME: indirect mov */;
                if (bo > 0)
                    asm_shr_imm(cg_sec, rt, 8, (uint8_t)(bo)); // movzbl (%s), %s
                if (bw < new_eff_sz * 8) {
                    if (node->lhs->member && (node->lhs->member->ty->is_unsigned || node->lhs->member->ty->is_enum)) {
                        unsigned long long m = (1ULL << bw) - 1;
                        asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(m)); // movzwl (%s), %s
                        asm_and_reg_reg(cg_sec, rt, 0, 8); // movl (%s), %s
                    } else {
                        int shift = 64 - bw;
                        asm_shl_imm(cg_sec, rt, 8, (uint8_t)(shift)); // movq (%s), %s
                        asm_sar_imm(cg_sec, rt, 8, (uint8_t)(shift)); // shr $%d, %s
                    }
                }
                free_reg(r2);
                int ret_reg = rt;
                free_reg(ra);
                return ret_reg;
            }
#endif
        }
        int r1 = gen_addr(node->lhs);
        int r2 = gen(node->rhs);
#ifdef ARCH_ARM64
        emit_store(node->lhs->ty, r2, r1, 0);
#else
        {
            int st_sz = node->lhs->ty->size;
            if (st_sz == 8) st_sz = 8;
            else if (st_sz == 4)
                st_sz = 4;
            else if (st_sz == 2)
                st_sz = 2;
            else
                st_sz = 1;
            X86Mem m = {CG_X86_REG(r1), X86_NOREG, 1, 0};
            x86_mov_mr(cg_sec, st_sz, m, CG_X86_REG(r2)); // mov %s, (%s)
        }
#endif
        free_reg(r1);
        return r2;
    }
    case ND_NEG: {
        int r = gen(node->lhs);
        if (is_flonum(node->ty)) {
#ifdef ARCH_ARM64
            (void)0 /* FIXME: fmov */;
            (void)0 /* FIXME: fneg/fabs */;
            (void)0 /* FIXME: fmov */;
#else
            // Negate float: xor with sign bit
            (void)0 /* TODO: movq to/from xmm */;
            // Use pxor with sign bit mask
            asm_mov_imm(cg_sec, r, 8, (long long)0x8000000000000000LL); // fmov d0, %s
            asm_movq_r_xmm(cg_sec, X86_XMM1, r); // fneg d0, d0
            x86_xorpd(cg_sec, X86_XMM0, X86_XMM1); // fmov %s, d0
            (void)0 /* TODO: movq to/from xmm */;
#endif
        } else {
#ifdef ARCH_ARM64
            (void)0 /* FIXME: neg 2op */;
#else
            x86_neg_r(cg_sec, node->lhs->ty->size, CG_X86_REG(r)); // movq %s, %%xmm0
#endif
        }
        return r;
    }
    case ND_NOT: {
        int r = gen(node->lhs);
        if (is_flonum(node->lhs->ty)) {
#ifdef ARCH_ARM64
            (void)0 /* FIXME: fmov */;
            (void)0 /* FIXME: fmov */;
            asm_fcmp(cg_sec, 1); // fmov d0, %s
            asm_cset(cg_sec, r, ARM64_EQ); // fmov d1, xzr
            (void)0 /* FIXME: cmov/csel/cneg */;
#else
            asm_movq_r_xmm(cg_sec, X86_XMM0, r); // fcmp d0, d1
            x86_pxor(cg_sec, X86_XMM1, X86_XMM1); // cset %s, eq
            asm_ucomisd(cg_sec); // csel %s, %s, wzr, vs
            asm_setcc(cg_sec, 1, X86_NP); // movq %s, %%xmm0
            asm_setcc(cg_sec, 0, X86_E); // xorpd %%xmm1, %%xmm1
            x86_and_rr(cg_sec, 1, X86_RAX, X86_RCX); // ucomisd %%xmm1, %%xmm0
            asm_movzx(cg_sec, r, 0, 4, 1); // setnp %%cl
#endif
        } else {
#ifdef ARCH_ARM64
            asm_cmp_zero(cg_sec, r, node->lhs->ty->size); // sete %%al
            asm_cset(cg_sec, r, ARM64_EQ); // andb %%cl, %%al
#else
            asm_cmp_zero(cg_sec, r, node->lhs->ty->size); // movzbl %%al, %s
            asm_setcc(cg_sec, 0, X86_E); // cmp %s, #0
            asm_movzx(cg_sec, r, 0, 4, 1); // cset %s, eq
#endif
        }
        return r;
    }
    case ND_ZERO_INIT: {
        // Zero-fill a local variable's stack memory
        LVar *var = node->lhs->var;
        if (!var || !var->is_local || var->ty->size <= 0) return -1;
        int c = ++rcc_label_count;
#ifdef ARCH_ARM64
        if (var->offset <= 4095) {
            (void)0 /* FIXME: sub with phy reg */;
        } else {
            emit_mov_imm64("x16", (uint64_t)var->offset);
            (void)0 /* FIXME: sub with phy reg */;
        }
        if (var->ty->size <= 4095) {
            asm_mov_imm(cg_sec, 9, 8, var->ty->size); // sub x11, %s, #%d
        } else {
            emit_mov_imm64("x12", (uint64_t)var->ty->size);
        }
        cg_def_label(format(".L.zero.%d", c)); // sub x11, %s, x16
        asm_cmp_zero(cg_sec, 9, 8); // mov x9, #%d
        size_t zj1 = asm_jcc_label(cg_sec, ARM64_EQ); // .L.zero.%d:
        asm_fixup_add(cg_sec, zj1, format(".L.zero_end.%d", c), 1); // cmp x9, #0
        asm_dec(cg_sec, 9, 8); // b.eq .L.zero_end.%d
        (void)0 /* FIXME: sized ld/st */;
        size_t zj2 = asm_jmp_label(cg_sec); // sub x9, x9, #1
        asm_fixup_add(cg_sec, zj2, format(".L.zero.%d", c), 0); // strb wzr, [x11, x9]
        cg_def_label(format(".L.zero_end.%d", c)); // b .L.zero.%d
#else
        asm_mov_imm(cg_sec, 1, 8, var->ty->size); // .L.zero_end.%d:
        cg_def_label(format(".L.zero.%d", c)); // movq $%d, %%rcx
        asm_cmp_zero(cg_sec, 1, 8); // .L.zero.%d:
        size_t zj1 = asm_jcc_label(cg_sec, X86_E); // cmpq $0, %%rcx
        asm_fixup_add(cg_sec, zj1, format(".L.zero_end.%d", c), 1); // je .L.zero_end.%d
        x86_mov_mi(cg_sec, 1, x86_mem_idx(X86_RBP, CG_X86_REG(1), 1, -var->offset - 1), 0); // movb $0, -%d-1(%%rbp,%%rcx)
        asm_dec(cg_sec, 1, 8); // subq $1, %%rcx
        size_t zj2 = asm_jmp_label(cg_sec); // jmp .L.zero.%d
        asm_fixup_add(cg_sec, zj2, format(".L.zero.%d", c), 0); // .L.zero_end.%d:
        cg_def_label(format(".L.zero_end.%d", c)); // sub x11, %s, #%d
#endif
        return -1;
    }
    case ND_POST_INC:
    case ND_POST_DEC: {
        int r = gen_addr(node->lhs);
        int r2 = alloc_reg();
        int sz = node->lhs->ty->size;
        // Handle bitfield post-increment/decrement with proper read-modify-write
        if (node->lhs->kind == ND_MEMBER && node->lhs->member &&
            node->lhs->member->bit_width > 0) {
            Member *mem = node->lhs->member;
            int bw = mem->bit_width;
            int bo = mem->bit_offset;
            int unit_sz = mem->bf_load_size ? mem->bf_load_size : mem->ty->size;
            unsigned long long mask = ((1ULL << bw) - 1) << bo;
            int eff_sz = unit_sz > 8 ? 8 : unit_sz;
#ifdef ARCH_ARM64
            // Load the full container word (unsigned)
            if (eff_sz == 1)
                (void)0 /* FIXME: sized ld/st */;
            else if (eff_sz == 2)
                (void)0 /* FIXME: sized ld/st */;
            else if (eff_sz == 4)
                asm_ldr_reg_off(cg_sec, r2, r, 4, 0); // ldrb %s, [%s]
            else
                asm_ldr_reg_off(cg_sec, r2, r, 8, 0); // ldrh %s, [%s]
            // Extract original bitfield value into r3 (for return)
            int r3 = alloc_reg();
            asm_mov_reg_reg(cg_sec, r3, r2, 8); // ldr %s, [%s]
            if (bo > 0)
                asm_shr_imm(cg_sec, r3, 8, (uint8_t)(reg64[r3])); // ldr %s, [%s]
            int load_bits = eff_sz * 8;
            if (bw < load_bits) {
                if (mem->ty->is_unsigned || mem->ty->is_enum) {
                    emit_mov_imm64("x16", (1ULL << bw) - 1);
                    asm_and_reg_reg(cg_sec, r3, 16, 8); // mov %s, %s
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r3, 8, (uint8_t)(reg64[r3])); // lsr %s, %s, #%d
                    asm_sar_imm(cg_sec, r3, 8, (uint8_t)(reg64[r3])); // and %s, %s, x16
                }
            }
            // Clear the field bits in container word (r2)
            emit_mov_imm64("x16", ~mask);
            asm_and_reg_reg(cg_sec, r2, 16, 8); // lsl %s, %s, #%d
            // Compute new field value in rn
            int rn = alloc_reg();
            asm_mov_reg_reg(cg_sec, rn, r3, 8); // asr %s, %s, #%d
            emit_mov_imm64("x16", (1ULL << bw) - 1);
            asm_and_reg_reg(cg_sec, rn, 16, 8); // and %s, %s, x16
            if (node->kind == ND_POST_INC)
                asm_add_imm(cg_sec, rn, 8, 1); // mov %s, %s
            else
                asm_sub_imm(cg_sec, rn, 8, 1); // and %s, %s, x16
            emit_mov_imm64("x16", (1ULL << bw) - 1);
            asm_and_reg_reg(cg_sec, rn, 16, 8); // add %s, %s, #1
            if (bo > 0)
                asm_shl_imm(cg_sec, rn, 8, (uint8_t)(reg64[rn])); // sub %s, %s, #1
            asm_or_reg_reg(cg_sec, r2, r2, 8); // and %s, %s, x16
            // Store
            if (eff_sz == 1)
                (void)0 /* FIXME: sized ld/st */;
            else if (eff_sz == 2)
                (void)0 /* FIXME: sized ld/st */;
            else if (eff_sz == 4)
                asm_str_reg_off(cg_sec, r2, r, 4, 0); // lsl %s, %s, #%d
            else
                asm_str_reg_off(cg_sec, r2, r, 8, 0); // orr %s, %s, %s
#else
            // Load the full container word (unsigned)
            if (eff_sz == 1)
                (void)0 /* FIXME: indirect ext */;
            else if (eff_sz == 2)
                (void)0 /* FIXME: indirect ext */;
            else if (eff_sz == 4)
                (void)0 /* FIXME: indirect mov */;
            else
                (void)0 /* FIXME: indirect mov */;
            // Extract original bitfield value into r3 (for return)
            int r3 = alloc_reg();
            asm_mov_reg_reg(cg_sec, r2, r3, 8); // strb %s, [%s]
            if (bo > 0)
                asm_shr_imm(cg_sec, r3, 8, (uint8_t)(bo)); // strh %s, [%s]
            int load_bits = eff_sz * 8;
            if (bw < load_bits) {
                if (mem->ty->is_unsigned || mem->ty->is_enum) {
                    unsigned long long m = (1ULL << bw) - 1;
                    asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(m)); // str %s, [%s]
                    asm_and_reg_reg(cg_sec, r3, 0, 8); // str %s, [%s]
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r3, 8, (uint8_t)(shift)); // movzbl (%s), %s
                    asm_sar_imm(cg_sec, r3, 8, (uint8_t)(shift)); // movzwl (%s), %s
                }
            }
            // Clear the field bits in container word (r2)
            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(~mask)); // movl (%s), %s
            asm_and_reg_reg(cg_sec, r2, 0, 8); // movq (%s), %s
            // Compute new field value in rn
            int rn = alloc_reg();
            asm_mov_reg_reg(cg_sec, r3, rn, 8); // mov %s, %s
            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)((1ULL << bw) - 1)); // shr $%d, %s
            asm_and_reg_reg(cg_sec, rn, 0, 8); // movabsq $%llu, %%rax
            if (node->kind == ND_POST_INC)
                x86_add_ri(cg_sec, 8, CG_X86_REG(rn), 1); // andq %%rax, %s
            else
                x86_sub_ri(cg_sec, 8, CG_X86_REG(rn), 1); // shl $%d, %s
            asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)((1ULL << bw) - 1)); // sar $%d, %s
            asm_and_reg_reg(cg_sec, rn, 0, 8); // movabsq $%llu, %%rax
            if (bo > 0)
                asm_shl_imm(cg_sec, rn, 8, (uint8_t)(bo)); // andq %%rax, %s
            asm_or_reg_reg(cg_sec, rn, r2, 8); // mov %s, %s
            // Store
            if (eff_sz == 1)
                (void)0 /* FIXME: sized mov */;
            else if (eff_sz == 2)
                (void)0 /* FIXME: sized mov */;
            else if (eff_sz == 4)
                x86_mov_mr(cg_sec, 4, x86_mem(CG_X86_REG(r), 0), CG_X86_REG(rn)); // movabsq $%llu, %%rax
            else
                x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(r), 0), CG_X86_REG(rn)); // andq %%rax, %s
#endif
            free_reg(rn);
            free_reg(r3);
            free_reg(r2);
            free_reg(r);
            return r3; // Return original value (post-increment semantics)
        }
#ifdef ARCH_ARM64
        // Load current value (correct load width for type)
        emit_load(node->lhs->ty, r2, r, 0);
        // Update in-place: load into temp, add/sub, store back
        int r3 = alloc_reg();
        emit_load(node->lhs->ty, r3, r, 0);
        if (is_flonum(node->lhs->ty)) {
            // Float post-inc/dec: use fp arithmetic via d0/d1
            int id = add_float_literal(1.0, sz);
            int tmp = alloc_reg();
            emit_adrp_add(reg64[tmp], format(".LF%d", id));
            if (sz == 4) {
                asm_ldr_fp(cg_sec, 1, tmp, 4); // fmov d0, %s
                secbuf_emit32le(cg_sec, node->kind == ND_POST_INC ? arm64_fadd(0, 0, 0, 1) : arm64_fsub(0, 0, 0, 1)); // ldr s1, .LF%d
            } else {
                asm_ldr_fp(cg_sec, 1, tmp, 8); // %s s0, s0, s1\n", node->kind == ND_POST_INC ? "fadd" : "fsub
                secbuf_emit32le(cg_sec, node->kind == ND_POST_INC ? arm64_fadd(1, 0, 0, 1) : arm64_fsub(1, 0, 0, 1)); // ldr d1, .LF%d
            }
            free_reg(tmp);
            asm_fmov_f2i(cg_sec, r3, 0, 1); // %s d0, d0, d1\n", node->kind == ND_POST_INC ? "fadd" : "fsub
        } else {
            int delta = 1;
            if (node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY)
                delta = node->lhs->ty->base->size;
            if (node->kind == ND_POST_INC)
                asm_add_imm(cg_sec, r3, sz, reg(r3, sz)); // fmov %s, d0
            else
                asm_sub_imm(cg_sec, r3, sz, reg(r3, sz)); // add %s, %s, #%d
        }
        emit_store(node->lhs->ty, r3, r, 0);
        free_reg(r3);
#else
        // x86_64: load from [r] into r2 with proper extension /* sub %s, %s, #%d\n */
        x86_mov_rm(cg_sec, sz, CG_X86_REG(r2), x86_mem(CG_X86_REG(r), 0)); // mov (%s), %s
        if (sz < 4) {
            if (node->lhs->ty->is_unsigned)
                x86_movzx_rm(cg_sec, 4, sz, CG_X86_REG(r2), x86_mem(CG_X86_REG(r), 0)); // movq %s, %%xmm0
            else
                x86_movsx_rm(cg_sec, 4, sz, CG_X86_REG(r2), x86_mem(CG_X86_REG(r), 0)); // fmov d0, %s
        }
        bool is_float = is_flonum(node->lhs->ty);
        if (is_float) {
            int id = add_float_literal(1.0, sz);
            if (sz == 4) {
                asm_lea_rip_reg(cg_sec, r2, format(".LF%d", id));
                x86_movss_rm(cg_sec, X86_XMM1, x86_mem(CG_X86_REG(r2), 0));
                if (node->kind == ND_POST_INC) x86_addss(cg_sec, X86_XMM0, X86_XMM1);
                else x86_subss(cg_sec, X86_XMM0, X86_XMM1);
            } else {
                asm_lea_rip_reg(cg_sec, r2, format(".LF%d", id));
                x86_movsd_rm(cg_sec, X86_XMM1, x86_mem(CG_X86_REG(r2), 0));
                if (node->kind == ND_POST_INC) x86_addsd(cg_sec, X86_XMM0, X86_XMM1);
                else x86_subsd(cg_sec, X86_XMM0, X86_XMM1);
            }
            (void)0 /* FIXME: store xmm0 back to memory */;
        } else {
            int delta = 1;
            if (node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY)
                delta = node->lhs->ty->base->size;
            int r3 = alloc_reg();
            asm_mov_reg_reg(cg_sec, r3, r2, sz > 4 ? 8 : 4); // movss %%xmm0, (%s)
            if (node->kind == ND_POST_INC)
                asm_add_imm(cg_sec, r3, sz, delta); // movsd %%xmm0, (%s)
            else
                asm_sub_imm(cg_sec, r3, sz, delta); // add%c $%d, (%s)
            x86_mov_mr(cg_sec, sz, x86_mem(CG_X86_REG(r), 0), CG_X86_REG(r3)); // sub%c $%d, (%s)
            free_reg(r3);
        }
#endif
        free_reg(r);
        return r2;
    }
    case ND_MEMBER: {
        int r = gen_addr(node);
        if (node->ty->kind == TY_ARRAY || node->ty->kind == TY_VLA) {
            return r; // array/VLA decays to pointer
        }
        Type *load_ty = (node->member && node->member->bit_width > 0) ? node->member->ty : node->ty;
        if (is_flonum(load_ty)) {
#ifdef ARCH_ARM64
            if (load_ty->size == 4) {
                asm_ldr_fp(cg_sec, 0, r, 4); // ldr s0, [%s]
                asm_fcvt(cg_sec, 1, 0, 0, 0); // fcvt d0, s0
            } else {
                asm_ldr_fp(cg_sec, 0, r, 8); // ldr d0, [%s]
            }
            asm_fmov_f2i(cg_sec, r, 0, 1); // fmov %s, d0
#else
            if (load_ty->size == 4) {
                x86_movss_rm(cg_sec, X86_XMM0, x86_mem(CG_X86_REG(r), 0)); // movss (%s), %%xmm0
                asm_cvtss2sd(cg_sec); // cvtss2sd %%xmm0, %%xmm0
            } else {
                x86_movsd_rm(cg_sec, X86_XMM0, x86_mem(CG_X86_REG(r), 0)); // movq %%xmm0, %s
            }
            asm_movq_xmm_r(cg_sec, r, X86_XMM0); // movsd (%s), %%xmm0
#endif
        } else if (node->member && node->member->bit_width > 0 && node->member->bf_load_size) {
            int ls = node->member->bf_load_size;
            int bw = node->member->bit_width;
            int bo = node->member->bit_offset;
            int eff_ls = ls > 8 ? 8 : ls;
            int r_addr = -1;
            if (ls > 8 && bo + bw > 64) {
                r_addr = alloc_reg();
#ifdef ARCH_ARM64
                asm_mov_reg_reg(cg_sec, r_addr, r, 8); // mov %s, %s
#else
                asm_mov_reg_reg(cg_sec, r, r_addr, 8); // mov %s, %s
#endif
            }
#ifdef ARCH_ARM64
            if (eff_ls <= 2)
                (void)0 /* FIXME: sized ld/st */;
            else if (eff_ls == 4)
                asm_ldr_reg_off(cg_sec, r, r, 4, 0); // ldrh %s, [%s]
            else
                asm_ldr_reg_off(cg_sec, r, r, 8, 0); // ldr %s, [%s]
            if (bo > 0)
                asm_shr_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // ldr %s, [%s]
            if (r_addr >= 0) {
                int overflow = bo + bw - 64;
                int tmp = alloc_reg();
                (void)0 /* FIXME: sized ld/st */;
                (void)0 /* FIXME: and variant */;
                asm_shl_imm(cg_sec, tmp, 8, (uint8_t)(reg64[tmp])); // lsr %s, %s, #%d
                asm_or_reg_reg(cg_sec, r, r, 8); // ldrb %s, [%s, #8]
                free_reg(tmp);
                free_reg(r_addr);
            }
            int load_bits = ls * 8;
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    emit_mov_imm64("x16", mask);
                    asm_and_reg_reg(cg_sec, r, 16, 8); // and %s, %s, x16
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // lsl %s, %s, #%d
                    asm_sar_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // asr %s, %s, #%d
                }
            }
            return r;
#else
            {
                X86Mem m = {CG_X86_REG(r), X86_NOREG, 1, 0};
                x86_mov_rm(cg_sec, eff_ls, CG_X86_REG(r), m); // movzbl (%s), %s
            }
            if (bo > 0)
                asm_shr_imm(cg_sec, r, 8, (uint8_t)(bo)); // movzwl (%s), %s
            if (r_addr >= 0) {
                int overflow = bo + bw - 64;
                int tmp = alloc_reg();
                X86Mem m = {CG_X86_REG(r_addr), X86_NOREG, 1, 8};
                x86_movzx_rm(cg_sec, 8, 1, CG_X86_REG(tmp), m); // movl (%s), %s
                unsigned long long mask = (1ULL << overflow) - 1;
                asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(mask)); // movq (%s), %s
                asm_and_reg_reg(cg_sec, tmp, 0, 8); // shr $%d, %s
                asm_shl_imm(cg_sec, tmp, 8, (uint8_t)(64 - bo)); // movzbl 8(%s), %s
                asm_or_reg_reg(cg_sec, tmp, r, 8); // and $%d, %s
                free_reg(tmp);
                free_reg(r_addr);
            }
            int load_bits = ls * 8;
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(mask)); // movabsq $%llu, %%rax
                    asm_and_reg_reg(cg_sec, r, 0, 8); // andq %%rax, %s
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r, 8, (uint8_t)(shift)); // shl $%d, %s
                    asm_sar_imm(cg_sec, r, 8, (uint8_t)(shift)); // sar $%d, %s
                }
            }
            return r;
#endif
        } else {
#ifdef ARCH_ARM64
            emit_load(load_ty, r, r, 0);
#else
            emit_load(load_ty, r, r, 0);
#endif
        }
        if (node->member && node->member->bit_width > 0) {
            int bw = node->member->bit_width;
            int bo = node->member->bit_offset;
            int load_bits = node->member->bf_load_size
                ? node->member->bf_load_size * 8
                : node->member->ty->size * 8;
#ifdef ARCH_ARM64
            if (bo > 0)
                asm_shr_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // lsr %s, %s, #%d
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    emit_mov_imm64("x16", mask);
                    asm_and_reg_reg(cg_sec, r, 16, 8); // and %s, %s, x16
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // lsl %s, %s, #%d
                    asm_sar_imm(cg_sec, r, 8, (uint8_t)(reg64[r])); // asr %s, %s, #%d
                }
            }
#else
            if (bo > 0)
                asm_shr_imm(cg_sec, r, 8, (uint8_t)(bo)); // shr $%d, %s
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(mask)); // movabsq $%llu, %%rax
                    asm_and_reg_reg(cg_sec, r, 0, 8); // andq %%rax, %s
                } else {
                    int shift = 64 - bw;
                    asm_shl_imm(cg_sec, r, 8, (uint8_t)(shift)); // shl $%d, %s
                    asm_sar_imm(cg_sec, r, 8, (uint8_t)(shift)); // sar $%d, %s
                }
            }
#endif
        }
        return r;
    }
    case ND_ADDR:
        return gen_addr(node->lhs);
    case ND_CAST: {
        int r = gen(node->lhs);
        Type *from = node->lhs->ty;
        Type *to = node->ty;
        if (is_flonum(from) && is_integer(to)) {
#ifdef ARCH_ARM64
            (void)0 /* FIXME: fmov */;
            {
                const char *dst = (to->size >= 8) ? reg64[r] : reg32[r];
                if (to->is_unsigned)
                    (void)0 /* FIXME: float op */;
                else
                    (void)0 /* FIXME: float op */;
            }
#else
            (void)0 /* TODO: movq to/from xmm */;
            if (to->size == 8 && to->is_unsigned) {
                int c = ++rcc_label_count;
                x86_movabs(cg_sec, X86_RAX, 0x43f0000000000000ULL); // fmov d0, %s
                x86_movq_r_xmm(cg_sec, X86_XMM1, X86_RAX); // fcvtzu %s, d0
                x86_subsd(cg_sec, X86_XMM0, X86_XMM1); // fcvtzs %s, d0
                asm_jcc_label(cg_sec, X86_B); // movq %s, %%xmm0
                x86_addsd(cg_sec, X86_XMM0, X86_XMM1); // movq $0x43e0000000000000, %%rax
                asm_cvttsd2si(cg_sec, r, 8); // movq %%rax, %%xmm1
                x86_movabs(cg_sec, X86_RAX, 1ULL << 63); // comisd %%xmm1, %%xmm0
                x86_xor_rr(cg_sec, 8, CG_X86_REG(r), X86_RAX); // jb .L.ucast.%d
                asm_jmp_label(cg_sec); // subsd %%xmm1, %%xmm0
                cg_def_label(format(".L.u2f.high.%d", c)); // cvttsd2si %%xmm0, %s
                asm_cvttsd2si(cg_sec, r, 8); // movq $0x8000000000000000, %%rcx
                cg_def_label(format(".L.u2f.end.%d", c)); // orq %%rcx, %s
            } else if (to->size <= 4 && to->is_unsigned) {
                // float-to-unsigned-int: cvttsd2si is signed, so handle [2^31, 2^32) range.
                int c = ++rcc_label_count;
                x86_movabs(cg_sec, X86_RAX, 0x41f0000000000000ULL); // jmp .L.ucast_end.%d
                x86_movq_r_xmm(cg_sec, X86_XMM1, X86_RAX); // .L.ucast.%d:
                x86_subsd(cg_sec, X86_XMM0, X86_XMM1); // cvttsd2si %%xmm0, %s
                asm_jcc_label(cg_sec, X86_B); // .L.ucast_end.%d:
                x86_addsd(cg_sec, X86_XMM0, X86_XMM1); // movq $0x41e0000000000000, %%rax
                asm_cvttsd2si(cg_sec, r, 4); // movq %%rax, %%xmm1
                x86_add_ri(cg_sec, 4, CG_X86_REG(r), 1 << 31); // comisd %%xmm1, %%xmm0
                asm_jmp_label(cg_sec); // jb .L.ucast32.%d
                cg_def_label(format(".L.u2f.high.%d", c)); // subsd %%xmm1, %%xmm0
                asm_cvttsd2si(cg_sec, r, 4); // cvttsd2si %%xmm0, %s
                cg_def_label(format(".L.u2f.end.%d", c)); // addl $0x80000000, %s
            } else if (to->size <= 4 && !to->is_unsigned) {
                int c = ++rcc_label_count;
                asm_cvttsd2si(cg_sec, r, 4); // jmp .L.ucast32_end.%d
                x86_cmp_ri(cg_sec, 4, CG_X86_REG(r), 0x80000000); // .L.ucast32.%d:
                asm_jcc_label(cg_sec, X86_NE); // cvttsd2si %%xmm0, %s
                x86_xorpd(cg_sec, X86_XMM1, X86_XMM1); // .L.ucast32_end.%d:
                asm_ucomisd(cg_sec); // cvttsd2si %%xmm0, %s
                asm_jcc_label(cg_sec, X86_B); // cmp $0x80000000, %s
                x86_mov_ri(cg_sec, 4, CG_X86_REG(r), 0x7fffffff); // jne .L.sat_end.%d
                cg_def_label(format(".L.u2f.high.%d", c)); // xorpd %%xmm1, %%xmm1
            } else {
                x86_cvttsd2si(cg_sec, to->size, CG_X86_REG(r), X86_XMM0); // comisd %%xmm1, %%xmm0
            }
#endif
        } else if (is_integer(from) && is_flonum(to)) {
#ifdef ARCH_ARM64
            if (from->is_unsigned) {
                (void)0 /* FIXME: float op */;
            } else {
                (void)0 /* FIXME: float op */;
            }
            if (to->kind == TY_FLOAT) {
                (void)0 /* arm64 fcvt s0,d0 */;
                (void)0 /* arm64 fcvt d0,s0 */;
            }
            (void)0 /* FIXME: fmov */;
#else
            if (from->is_unsigned && from->size == 8) {
                int c = ++rcc_label_count;
                asm_test_reg_reg(cg_sec, r, r, 8); // jb .L.sat_end.%d
                asm_jcc_label(cg_sec, X86_S); // mov $0x7fffffff, %s
                asm_cvtsi2sd(cg_sec, r, 8); // .L.sat_end.%d:
                asm_jmp_label(cg_sec); // cvttsd2si %%xmm0, %s
                cg_def_label(format(".L.u2f.high.%d", c)); // ucvtf d0, %s
                asm_mov_reg_reg(cg_sec, 1, r, 8); // scvtf d0, %s
                asm_shl_cl(cg_sec, 1, 8); // fcvt s0, d0
                asm_cvtsi2sd(cg_sec, 1, 8); // fcvt d0, s0
                x86_addsd(cg_sec, X86_XMM0, X86_XMM1); // fmov %s, d0
                cg_def_label(format(".L.u2f.end.%d", c)); // testq %s, %s
            } else if (from->is_unsigned && from->size == 4) {
                asm_cvtsi2sd(cg_sec, r, 8); // js .L.u2f.high.%d
            } else {
                x86_cvtsi2sd(cg_sec, from->size, X86_XMM0, CG_X86_REG(r)); // cvtsi2sd %s, %%xmm0
            }
            if (to->kind == TY_FLOAT) {
                asm_cvtsd2ss(cg_sec); // jmp .L.u2f.end.%d
                asm_cvtss2sd(cg_sec); // .L.u2f.high.%d:
            }
            (void)0 /* TODO: movq to/from xmm */;
#endif
        } else if (is_flonum(from) && is_flonum(to)) {
#ifdef ARCH_ARM64
            if (to->kind == TY_FLOAT && from->kind != TY_FLOAT) {
                (void)0 /* FIXME: fmov */;
                (void)0 /* arm64 fcvt s0,d0 */;
                (void)0 /* arm64 fcvt d0,s0 */;
                (void)0 /* FIXME: fmov */;
            }
#else
            if (to->kind == TY_FLOAT && from->kind != TY_FLOAT) {
                (void)0 /* TODO: movq to/from xmm */;
                asm_cvtsd2ss(cg_sec); // movq %s, %%rcx
                asm_cvtss2sd(cg_sec); // shrq %%rcx
                (void)0 /* TODO: movq to/from xmm */;
            }
#endif
        } else if (to->size == 1) {
#ifdef ARCH_ARM64
            if (to->is_unsigned)
                (void)0 /* FIXME: and variant */;
            else
                asm_movsx(cg_sec, r, r, 4, 1); // cvtsi2sd %%rcx, %%xmm0
#else
            if (to->is_unsigned)
                asm_movzx(cg_sec, r, r, 4, 1); // addsd %%xmm0, %%xmm0
            else
                asm_movsx(cg_sec, r, r, 4, 1); // .L.u2f.end.%d:
#endif
        } else if (to->size == 2) {
#ifdef ARCH_ARM64
            if (to->is_unsigned)
                (void)0 /* FIXME: and variant */;
            else
                asm_movsx(cg_sec, r, r, 4, 2); // cvtsi2sd %s, %%xmm0
#else
            if (to->is_unsigned)
                asm_movzx(cg_sec, r, r, 4, 2); // cvtsi2sd %s, %%xmm0
            else
                asm_movsx(cg_sec, r, r, 4, 2); // cvtsd2ss %%xmm0, %%xmm0
#endif
        } else if (to->size == 4 && from->size == 8) {
            asm_mov_reg_reg(cg_sec, r, r, 4); // cvtss2sd %%xmm0, %%xmm0
        } else if (to->size == 8 && from->size < 8) {
            if (from->is_unsigned)
                zero_extend_to(r, from->size, 8);
            else
                sign_extend_to(r, from->size, 8);
        }
        return r;
    }
    case ND_BITNOT: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        asm_not(cg_sec, r, 8 if 1 == "1" else 4); // mvn %s, %s
#else
        x86_not_r(cg_sec, node->ty->size, CG_X86_REG(r)); // not %s
#endif
        return r;
    }
    case ND_STR: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        emit_adrp_add(reg64[r], format(".LC%d", node->str_id));
#else
        asm_lea_rip_reg(cg_sec, r, format(".LC%d", node->str_id)); // lea .LC%d(%%rip), %s
#endif
        return r;
    }
    case ND_DEREF: {
        if (node->ty->kind == TY_FUNC || node->ty->kind == TY_ARRAY || node->ty->kind == TY_VLA)
            return gen(node->lhs);
        int r = gen(node->lhs);
        if (is_flonum(node->ty)) {
#ifdef ARCH_ARM64
            if (node->ty->size == 4) {
                asm_ldr_fp(cg_sec, 0, r, 4); // ldr s0, [%s]
                asm_fcvt(cg_sec, 1, 0, 0, 0); // fcvt d0, s0
            } else {
                asm_ldr_fp(cg_sec, 0, r, 8); // ldr d0, [%s]
            }
            asm_fmov_f2i(cg_sec, r, 0, 1); // fmov %s, d0
#else
            if (node->ty->size == 4) {
                x86_movss_rm(cg_sec, X86_XMM0, x86_mem(CG_X86_REG(r), 0)); // movss (%s), %%xmm0
                asm_cvtss2sd(cg_sec); // cvtss2sd %%xmm0, %%xmm0
            } else {
                x86_movsd_rm(cg_sec, X86_XMM0, x86_mem(CG_X86_REG(r), 0)); // movsd (%s), %%xmm0
            }
            asm_movq_xmm_r(cg_sec, r, X86_XMM0); // movq %%xmm0, %s
#endif
        } else {
#ifdef ARCH_ARM64
            emit_load(node->ty, r, r, 0);
#else
            emit_load(node->ty, r, r, 0);
#endif
        }
        return r;
    }
    case ND_RETURN: {
        if (node->lhs) {
            if (node->lhs->ty && (node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION)) {
                int src = gen_addr(node->lhs);
                int c = ++rcc_label_count;
                int retbuf_offset = 0;
                for (LVar *var = current_fn_def->locals; var; var = var->next) {
                    if (var->name && strcmp(var->name, "__retbuf") == 0) {
                        retbuf_offset = var->offset;
                        break;
                    }
                }
#ifdef ARCH_ARM64
                if (retbuf_offset <= 4095)
                    (void)0 /* FIXME: ldr/str phy/off */;
                else {
                    int v = retbuf_offset;
                    asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // ldr x11, [%s, #-%d]
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // mov x16, #%d
                        v >>= 16;
                        s += 16;
                    }
                    (void)0 /* FIXME: sub with phy reg */;
                }
                asm_mov_imm(cg_sec, 9, 8, node->lhs->ty->size); // movk x16, #%d, lsl #%d
                (void)0 /* FIXME: label .L.xxx.c */;
                asm_cmp_zero(cg_sec, 9, 8); // sub x11, %s, x16
                asm_jcc_label(cg_sec, ARM64_EQ); // mov x9, #%d
                asm_dec(cg_sec, 9, 8); // .L.retcopy.%d:
                (void)0 /* FIXME: sized ld/st */;
                (void)0 /* FIXME: sized ld/st */;
                asm_jmp_label(cg_sec); // cmp x9, #0
                (void)0 /* FIXME: label .L.xxx.c */;
                (void)0 /* FIXME: mov phy */;
#else
                asm_mov_rbp_phyreg(cg_sec, X86_R11, 8, retbuf_offset); // b.eq .L.retcopy_end.%d
                asm_mov_imm(cg_sec, 1, 8, node->lhs->ty->size); // sub x9, x9, #1
                (void)0 /* FIXME: label .L.xxx.c */;
                asm_cmp_zero(cg_sec, 1, 8); // ldrb w16, [%s, x9]
                asm_jcc_label(cg_sec, X86_E); // strb w16, [x11, x9]
                (void)0 /* FIXME: sized mov */;
                (void)0 /* FIXME: sized mov */;
                asm_dec(cg_sec, 1, 8); // b .L.retcopy.%d
                asm_jmp_label(cg_sec); // .L.retcopy_end.%d:
                (void)0 /* FIXME: label .L.xxx.c */;
                asm_mov_reg_reg(cg_sec, 0, 11, 8); // mov x0, x11
#endif
                free_reg(src);
            } else {
                int r = gen(node->lhs);
                Type *ret_ty = current_fn_def->ty->return_ty;
                if (ret_ty && is_flonum(ret_ty)) {
                    if (node->lhs->ty && is_flonum(node->lhs->ty)) {
#ifdef ARCH_ARM64
                        (void)0 /* FIXME: fmov */;
                        if (ret_ty->kind == TY_FLOAT)
                            (void)0 /* arm64 fcvt s0,d0 */;
#else
                        (void)0 /* TODO: movq to/from xmm */;
                        if (ret_ty->kind == TY_FLOAT)
                            asm_cvtsd2ss(cg_sec); // fmov d0, %s
#endif
                    } else if (ret_ty->size == 4) {
#ifdef ARCH_ARM64
                        if (node->lhs->ty && node->lhs->ty->is_unsigned)
                            (void)0 /* FIXME: float op */;
                        else
                            (void)0 /* FIXME: float op */;
#else
                        {
                            int src_sz = node->lhs->ty ? node->lhs->ty->size : 4;
                            bool src_u = node->lhs->ty && node->lhs->ty->is_unsigned;
                            if (src_u && src_sz == 8) {
                                // unsigned long long → float: handle high bit
                                int c = ++rcc_label_count;
                                asm_test_reg_reg(cg_sec, r, r, 8); // testq %s, %s
                                asm_jcc_label(cg_sec, X86_S); // js .L.u2f.high.%d
                                x86_cvtsi2ss(cg_sec, 8, X86_XMM0, CG_X86_REG(r)); // cvtsi2ss %s, %%xmm0
                                asm_jmp_label(cg_sec); // jmp .L.u2f.end.%d
                                cg_def_label(format(".L.u2f.high.%d", c)); // .L.u2f.high.%d:
                                asm_mov_reg_reg(cg_sec, 1, r, 8); // movq %s, %%rcx
                                asm_shl_cl(cg_sec, 1, 8); // shrq %%rcx
                                x86_cvtsi2ss(cg_sec, 8, X86_XMM0, X86_RCX); // cvtsi2ss %%rcx, %%xmm0
                                x86_addss(cg_sec, X86_XMM0, X86_XMM1); // addss %%xmm0, %%xmm0
                                cg_def_label(format(".L.u2f.end.%d", c)); // .L.u2f.end.%d:
                            } else if (src_u && src_sz <= 4) {
                                // unsigned int/short/char → float: zero-extend to 64-bit,
                                // then cvtsi2ss with 64-bit reg (value is non-negative 64-bit int)
                                x86_cvtsi2ss(cg_sec, 8, X86_XMM0, CG_X86_REG(r)); // cvtsi2ss %s, %%xmm0
                            } else {
                                int cssz = (src_sz >= 8) ? 8 : (src_sz < 4 ? 4 : src_sz);
                                x86_cvtsi2ss(cg_sec, cssz, X86_XMM0, CG_X86_REG(r)); // cvtsi2ss %s, %%xmm0
                            }
                        }
#endif
                    } else {
#ifdef ARCH_ARM64
                        if (node->lhs->ty && node->lhs->ty->is_unsigned)
                            (void)0 /* FIXME: float op */;
                        else
                            (void)0 /* FIXME: float op */;
#else
                        {
                            int src_sz = node->lhs->ty ? node->lhs->ty->size : 8;
                            bool src_u = node->lhs->ty && node->lhs->ty->is_unsigned;
                            if (src_u && src_sz == 8) {
                                // unsigned long long → double: handle high bit
                                int c = ++rcc_label_count;
                                asm_test_reg_reg(cg_sec, r, r, 8); // testq %s, %s
                                asm_jcc_label(cg_sec, X86_S); // js .L.u2f.high.%d
                                asm_cvtsi2sd(cg_sec, r, 8); // cvtsi2sd %s, %%xmm0
                                asm_jmp_label(cg_sec); // jmp .L.u2f.end.%d
                                cg_def_label(format(".L.u2f.high.%d", c)); // .L.u2f.high.%d:
                                asm_mov_reg_reg(cg_sec, 1, r, 8); // movq %s, %%rcx
                                asm_shl_cl(cg_sec, 1, 8); // shrq %%rcx
                                asm_cvtsi2sd(cg_sec, 1, 8); // cvtsi2sd %%rcx, %%xmm0
                                x86_addsd(cg_sec, X86_XMM0, X86_XMM1); // addsd %%xmm0, %%xmm0
                                cg_def_label(format(".L.u2f.end.%d", c)); // .L.u2f.end.%d:
                            } else if (src_u && src_sz <= 4) {
                                // unsigned int/short/char → double: zero-extend to 64-bit
                                asm_cvtsi2sd(cg_sec, r, 8); // cvtsi2sd %s, %%xmm0
                            } else {
                                int cssz = (src_sz >= 8) ? 8 : (src_sz < 4 ? 4 : src_sz);
                                x86_cvtsi2sd(cg_sec, cssz, X86_XMM0, CG_X86_REG(r)); // cvtsi2sd %s, %%xmm0
                            }
                        }
#endif
                    }
                } else if (node->lhs->ty && is_flonum(node->lhs->ty)) {
                    // Float expression returned as integer: convert from xmm0/d0
                    int sz = ret_ty ? ret_ty->size : 8;
#ifdef ARCH_ARM64
                    {
                        const char *dst = (sz >= 8) ? reg64[r] : reg32[r];
                        bool ret_unsigned = ret_ty && ret_ty->is_unsigned;
                        if (ret_unsigned)
                            (void)0 /* FIXME: float op */;
                        else
                            (void)0 /* FIXME: float op */;
                    }
                    if (sz >= 8)
                        (void)0 /* FIXME: mov phy */;
                    else if (ret_ty && ret_ty->is_unsigned)
                        (void)0 /* FIXME: mov phy */;
                    else
                        asm_sxtw(cg_sec, 0, r); // fcvtzu %s, d0
#else
                    asm_cvttsd2si(cg_sec, r, sz); // fcvtzs %s, d0
                    x86_mov_rr(cg_sec, 8, X86_RAX, CG_X86_REG(r)); // mov x0, %s
#endif
                } else {
#ifdef ARCH_ARM64
                    // Truncate return value to match function return type width
                    if (ret_ty && ret_ty->size < 4) {
                        if (ret_ty->is_unsigned)
                            asm_and_imm(cg_sec, r, 4, (1 << (ret_ty->size * 8)) - 1);
                        else if (ret_ty->size == 1)
                            asm_movsx(cg_sec, r, r, 4, 1); // sxtb %s, %s
                        else
                            asm_movsx(cg_sec, r, r, 4, 2); // sxth %s, %s
                    }
                    (void)0 /* FIXME: mov phy */;
#else
                    x86_mov_rr(cg_sec, 8, X86_RAX, CG_X86_REG(r)); // mov x0, %s
                    // Truncate return value to match function return type width
                    if (ret_ty && ret_ty->size < 4 && ret_ty->is_unsigned)
                        asm_and_imm(cg_sec, r, 4, (1 << (ret_ty->size * 8)) - 1); // movq %s, %%rax
#endif
                }
                free_reg(r);
            }
        }
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        size_t ret_off = asm_jmp_label(cg_sec); /* jmp/b .L.return.%s */ /* andl $%d, %%eax\n */
        char *ret_lbl = format(".L.return.%s", current_fn_def->name);
        asm_fixup_add(cg_sec, ret_off, ret_lbl, 0); // b .L.return.%s
        return -1;
    }
    case ND_NULL:
        return -1;
    case ND_COMMA:
    case ND_CHAIN: {
        int r1 = gen(node->lhs);
        if (r1 != -1) free_reg(r1);
        return gen(node->rhs);
    }
    case ND_ALLOCA:
    case ND_ALLOCA_ZINIT: {
#ifdef ARCH_ARM64
        if (node->kind == ND_ALLOCA_ZINIT && node->lhs && node->lhs->kind == ND_NUM && node->lhs->val == 0) {
            arm64_load_from_fp_minus(node->var->offset, "x16");
            (void)0 /* FIXME: mov phy */;
            return -1;
        }
        int r = gen(node->lhs);
        // Save current SP into VLA save slot
        (void)0 /* FIXME: mov phy */;
        arm64_store_to_fp_minus("x16", node->var->offset);
        // Round size up to 16-byte alignment, keep in x16 (scratch, not in pool)
        asm_add_imm(cg_sec, r, 8, 15); // mov sp, x16
        asm_and_imm(cg_sec, r, 8, -16); // mov x16, sp
        (void)0 /* FIXME: mov phy */;
        free_reg(r);
        (void)0 /* FIXME: sub with phy reg */;
        if (node->kind == ND_ALLOCA_ZINIT) {
            (void)0 /* FIXME: alloca */;
            (void)0 /* FIXME: subs */;
            asm_jcc_label(cg_sec, ARM64_LT); // add %s, %s, #15
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_jmp_label(cg_sec); // and %s, %s, #-16
        } else {
            (void)0 /* FIXME: alloca */;
            (void)0 /* FIXME: subs */;
            asm_jcc_label(cg_sec, ARM64_LT); // mov x16, %s
            (void)0 /* FIXME: sized ld/st */;
            asm_jmp_label(cg_sec); // sub sp, sp, x16
        }
        (void)0 /* FIXME: alloca */;
        rcc_label_count++;
        // Save VLA data pointer
        (void)0 /* FIXME: mov phy */;
        arm64_store_to_fp_minus("x16", node->var->offset - 8);
#else
        if (node->kind == ND_ALLOCA_ZINIT && node->lhs && node->lhs->kind == ND_NUM && node->lhs->val == 0) {
            asm_mov_rbp_phyreg(cg_sec, X86_RSP, 8, node->var->offset); // .L.alloca.zero.%d:
            return -1;
        }
        int r = gen(node->lhs);
        asm_mov_phyreg_rbp(cg_sec, X86_RSP, 8, node->var->offset); // subs x16, x16, #8
        asm_mov_reg_reg(cg_sec, 0, r, 8); // b.lt .L.alloca.done.%d
        free_reg(r);
        x86_mov_rr(cg_sec, 8, X86_RCX, X86_RSP); // str xzr, [sp, x16]
        x86_add_ri(cg_sec, 8, X86_RAX, 15); // b .L.alloca.zero.%d
        int align = 16;
        x86_and_ri(cg_sec, 8, X86_RAX, -16); // .L.alloca.probe.%d:
        x86_sub_rr(cg_sec, 8, X86_RSP, X86_RAX); // subs x16, x16, #4096
        if (node->kind == ND_ALLOCA_ZINIT) {
            x86_xor_rr(cg_sec, 4, X86_RDX, X86_RDX); // b.lt .L.alloca.done.%d
            x86_test_rr(cg_sec, 8, X86_RAX, X86_RAX); // ldrb w17, [sp, x16]
            asm_jcc_label(cg_sec, X86_S); // b .L.alloca.probe.%d
            asm_movaps_rbp_xmm(cg_sec, X86_XMM0, 0); // .L.alloca.done.%d:
            asm_jmp_label(cg_sec); // mov x16, sp
        } else {
            x86_test_rr(cg_sec, 8, X86_RAX, X86_RAX); // movq -%d(%%rbp), %%rsp
            asm_jcc_label(cg_sec, X86_S); // movq %%rsp, -%d(%%rbp)
            x86_or_mi(cg_sec, 1, x86_mem_idx(X86_RSP, X86_RCX, 1, 0), 0); // movq %s, %%rax
            asm_jmp_label(cg_sec); // movq %%rsp, %%rcx
        }
        rcc_label_count++;
        if (node->var)
            asm_mov_phyreg_rbp(cg_sec, X86_RSP, 8, node->var->offset - 8); // subq %%rax, %%rsp
        else
            x86_mov_rr(cg_sec, 8, X86_RAX, X86_RSP); // andq $-%d, %%rsp
#endif
        return -1;
    }
    case ND_BLOCK:
        for (Node *n = node->body; n; n = n->next) {
            int r = gen(n);
            if (r != -1) free_reg(r);
        }
        return -1;
    case ND_STMT_EXPR: {
        int result = -1;
        for (Node *n = node->body; n; n = n->next) {
            if (node->stmt_expr_result && n->kind == ND_EXPR_STMT && n->lhs == node->stmt_expr_result) {
                result = gen(node->stmt_expr_result);
            } else {
                int r = gen(n);
                if (r != -1)
                    free_reg(r);
            }
        }
        if (result == -1) {
            result = alloc_reg();
#ifdef ARCH_ARM64
            asm_movq_zero(cg_sec, result); // mov %s, #0
#else
            asm_movl_zero(cg_sec, result); // mov $0, %s
#endif
        }
        return result;
    }
    case ND_EXPR_STMT: {
        int r = gen(node->lhs);
        if (r != -1) free_reg(r);
        return -1;
    }
    case ND_FUNCALL: {
        return gen_funcall(node, -1);
    }
    case ND_LOGAND: {
        int c = ++rcc_label_count;
        char false_label[32];
        sprintf(false_label, ".L.logand_false.%d", c);
#ifdef ARCH_ARM64
        int r = alloc_reg();
        int lhs = gen(node->lhs);
        asm_cmp_zero(cg_sec, lhs, node->lhs->ty->size); // cmp %s, #0
        asm_movq_zero(cg_sec, r); // mov %s, #0
        size_t ja1 = asm_jcc_label(cg_sec, ARM64_EQ); // b.eq .L.end.%d
        asm_fixup_add(cg_sec, ja1, false_label, 1); // cmp %s, #0
        free_reg(lhs);
        int rhs = gen(node->rhs);
        asm_cmp_zero(cg_sec, rhs, node->rhs->ty->size); // cset %s, ne
        asm_cset(cg_sec, r, ARM64_NE); // cmp $0, %s
#else
        int r = alloc_reg();
        int lhs = gen(node->lhs);
        asm_cmp_zero(cg_sec, lhs, node->lhs->ty->size); // movb $0, -%d(%%rbp)
        asm_movl_zero(cg_sec, r); // je .L.end.%d
        size_t ja1 = asm_jcc_label(cg_sec, X86_E); // cmp $0, %s
        asm_fixup_add(cg_sec, ja1, false_label, 1); // setne %%al
        free_reg(lhs);
        int rhs = gen(node->rhs);
        asm_cmp_zero(cg_sec, rhs, node->rhs->ty->size); // movb %%al, -%d(%%rbp)
        asm_setcc(cg_sec, r, X86_NE); // .L.end.%d:
        asm_movzx(cg_sec, r, r, 4, 1); // movzbl -%d(%%rbp), %s
#endif
        free_reg(rhs);
        cg_def_label(false_label); // cmp %s, #0
        return r;
    }
    case ND_LOGOR: {
        int c = ++rcc_label_count;
        char true_label[32], end_label[32];
        sprintf(true_label, ".L.logor_true.%d", c);
        sprintf(end_label, ".L.logor_end.%d", c);
#ifdef ARCH_ARM64
        int r = alloc_reg();
        int lhs = gen(node->lhs);
        asm_cmp_zero(cg_sec, lhs, node->lhs->ty->size); // mov %s, #1
        asm_mov_imm(cg_sec, r, 4, 1); // b.ne .L.end.%d
        size_t jo1 = asm_jcc_label(cg_sec, ARM64_NE); // cmp %s, #0
        asm_fixup_add(cg_sec, jo1, true_label, 1); // cset %s, ne
        free_reg(lhs);
        int rhs = gen(node->rhs);
        asm_cmp_zero(cg_sec, rhs, node->rhs->ty->size); // cmp $0, %s
        asm_cset(cg_sec, r, ARM64_NE); // movb $1, -%d(%%rbp)
        size_t jo2 = asm_jmp_label(cg_sec); // jne .L.end.%d
        asm_fixup_add(cg_sec, jo2, end_label, 0); // cmp $0, %s
        cg_def_label(true_label); // setne %%al
#else
        int r = alloc_reg();
        int lhs = gen(node->lhs);
        asm_cmp_zero(cg_sec, lhs, node->lhs->ty->size); // movb %%al, -%d(%%rbp)
        asm_mov_imm(cg_sec, r, 4, 1); // .L.end.%d:
        size_t jo1 = asm_jcc_label(cg_sec, X86_NE); // movzbl -%d(%%rbp), %s
        asm_fixup_add(cg_sec, jo1, true_label, 1); // cmp %s, #0
        free_reg(lhs);
        int rhs = gen(node->rhs);
        asm_cmp_zero(cg_sec, rhs, node->rhs->ty->size); // b.eq .L.else.%d
        asm_setcc(cg_sec, r, X86_NE); // mov %s, %s
        asm_movzx(cg_sec, r, r, 4, 1); // b .L.end.%d
        size_t jo2 = asm_jmp_label(cg_sec); // .L.else.%d:
        asm_fixup_add(cg_sec, jo2, end_label, 0); // mov %s, %s
        cg_def_label(true_label); // cmp $0, %s
#endif
        free_reg(rhs);
        cg_def_label(end_label); // je .L.else.%d
        return r;
    }
    case ND_COND: {
        int c = ++rcc_label_count;
        char else_label[32], end_label[32];
        sprintf(else_label, ".L.cond_else.%d", c);
        sprintf(end_label, ".L.cond_end.%d", c);
        int r = alloc_reg();
        int cond = gen(node->cond);
#ifdef ARCH_ARM64
        asm_cmp_zero(cg_sec, cond, node->cond->ty->size); // mov %s, %s
        size_t cj1 = asm_jcc_label(cg_sec, ARM64_EQ); // jmp .L.end.%d
        asm_fixup_add(cg_sec, cj1, else_label, 1); // .L.else.%d:
        free_reg(cond);
        int then_r = gen(node->then);
        asm_mov_reg_reg(cg_sec, r, then_r, 8); // mov %s, %s
        free_reg(then_r);
        size_t cj2 = asm_jmp_label(cg_sec); // .L.end.%d:
        asm_fixup_add(cg_sec, cj2, end_label, 0); // cmp %s, #0
        cg_def_label(else_label); // mov %s, #0
        int else_r = gen(node->els);
        asm_mov_reg_reg(cg_sec, r, else_r, 8); // b.eq .L.end.%d
        free_reg(else_r);
#else
        asm_cmp_zero(cg_sec, cond, node->cond->ty->size); // cmp %s, #0
        size_t cj1 = asm_jcc_label(cg_sec, X86_E); // cset %s, ne
        asm_fixup_add(cg_sec, cj1, else_label, 1); // cmp $0, %s
        free_reg(cond);
        int then_r = gen(node->then);
        asm_mov_reg_reg(cg_sec, r, then_r, 8); // movb $0, -%d(%%rbp)
        free_reg(then_r);
        size_t cj2 = asm_jmp_label(cg_sec); // je .L.end.%d
        asm_fixup_add(cg_sec, cj2, end_label, 0); // cmp $0, %s
        cg_def_label(else_label); // setne %%al
        int else_r = gen(node->els);
        asm_mov_reg_reg(cg_sec, r, else_r, 8); // movb %%al, -%d(%%rbp)
        free_reg(else_r);
#endif
        cg_def_label(end_label); // .L.end.%d:
        return r;
    }
    case ND_IF: {
        // Fold constant integer conditions to avoid dead code emission,
        // but keep blocks that contain labels (may be targets of goto).
        if (node->cond->kind == ND_NUM) {
            // Recursively check if a node tree contains any label or goto
            bool has_label = false;
            {
                Node *stack[512];
                int sp = 0;
                for (Node *n = node->then; n && sp < 512; n = n->next)
                    stack[sp++] = n;
                if (node->els)
                    for (Node *n = node->els; n && sp < 512; n = n->next)
                        stack[sp++] = n;
                while (sp > 0 && !has_label) {
                    Node *n = stack[--sp];
                    if (n->kind == ND_LABEL || n->kind == ND_GOTO || n->kind == ND_GOTO_IND ||
                        n->kind == ND_CASE || n->kind == ND_LABEL_VAL) {
                        has_label = true;
                    } else if (n->kind == ND_FOR || n->kind == ND_DO || n->kind == ND_IF) {
                        if (n->then && sp < 512) stack[sp++] = n->then;
                        if (n->cond && sp < 512) stack[sp++] = n->cond;
                        if (n->kind == ND_FOR) {
                            if (n->body && sp < 512) stack[sp++] = n->body;
                            if (n->init && sp < 512) stack[sp++] = n->init;
                            if (n->inc && sp < 512) stack[sp++] = n->inc;
                        }
                        if (n->els && sp < 512) stack[sp++] = n->els;
                    } else if (n->kind == ND_BLOCK) {
                        for (Node *c = n->body; c && sp < 512; c = c->next)
                            stack[sp++] = c;
                    }
                }
            }
            if (!has_label) {
                if (node->cond->val) {
                    int r = gen(node->then);
                    if (r != -1) free_reg(r);
                } else if (node->els) {
                    int r = gen(node->els);
                    if (r != -1) free_reg(r);
                }
                return -1;
            }
            // if(0) with labels: skip dead non-label nodes before the
            // first label (Duff device pattern where labels are reachable
            // via switch/goto).  Plain if(1) with labels falls through.
            if (!node->cond->val) {
                // Find outermost block body
                Node *list = node->then;
                while (list && list->kind == ND_BLOCK && list->body &&
                       list->body->kind == ND_BLOCK && !list->body->next)
                    list = list->body;
                if (list && list->kind == ND_BLOCK)
                    list = list->body;
                // Skip to first label/case
                Node *n = list;
                while (n && n->kind != ND_LABEL && n->kind != ND_CASE && n->kind != ND_LABEL_VAL)
                    n = n->next;
                // If the first label is a case (Duff), skip dead code before it.
                // Otherwise generate full body to preserve goto labels.
                if (n && n->kind == ND_CASE) {
                    for (; n; n = n->next) {
                        int r = gen(n);
                        if (r != -1) free_reg(r);
                    }
                    return -1;
                }
            }
        }
        int c = ++rcc_label_count;
        char end_label[32], else_label[32];
        sprintf(end_label, ".L.end.%d", c);
        sprintf(else_label, ".L.else.%d", c);

        if (node->els) {
            gen_cond_branch_inv(node->cond, else_label);
            int r1 = gen(node->then);
            if (r1 != -1) free_reg(r1);
#ifdef ARCH_ARM64
            size_t if_jmp_off = asm_jmp_label(cg_sec); // b %s
            asm_fixup_add(cg_sec, if_jmp_off, end_label, 0); // jmp %s
#else
            size_t if_jmp_off = asm_jmp_label(cg_sec); // %s:
            asm_fixup_add(cg_sec, if_jmp_off, end_label, 0); // %s:
#endif
            cg_def_label(else_label); // %s:
            int r2 = gen(node->els);
            if (r2 != -1) free_reg(r2);
            cg_def_label(end_label); // b %s
        } else {
            gen_cond_branch_inv(node->cond, end_label);
            int r1 = gen(node->then);
            if (r1 != -1) free_reg(r1);
            cg_def_label(end_label); // jmp %s
        }
        return -1;
    }
    case ND_FOR: {
        int c = ++rcc_label_count;
        char begin_label[32], end_label[32], cont_label[32];
        sprintf(begin_label, ".L.begin.%d", c);
        sprintf(end_label, ".L.end.%d", c);
        sprintf(cont_label, ".L.continue.%d", c);

        if (node->init) {
            int r = gen(node->init);
            if (r != -1) free_reg(r);
        }
        break_stack[ctrl_depth] = c;
        continue_stack[ctrl_depth] = c;
        ctrl_depth++;
        cg_def_label(begin_label); // %s:
        if (node->cond) {
            gen_cond_branch_inv(node->cond, end_label);
        }
        int r_then = gen(node->then);
        if (r_then != -1) free_reg(r_then);
        cg_def_label(cont_label); // %s:
        if (node->inc) {
            int r_inc = gen(node->inc);
            if (r_inc != -1) free_reg(r_inc);
        }
#ifdef ARCH_ARM64
        size_t for_jmp_off = asm_jmp_label(cg_sec); // b %s
        asm_fixup_add(cg_sec, for_jmp_off, begin_label, 0); // jmp %s
#else
        size_t for_jmp_off = asm_jmp_label(cg_sec); // %s:
        asm_fixup_add(cg_sec, for_jmp_off, begin_label, 0); // .L.begin.%d:
#endif
        cg_def_label(end_label); // .L.continue.%d:
        ctrl_depth--;
        return -1;
    }
    case ND_DO: {
        int c = ++rcc_label_count;
        char begin_label[32], end_label[32], cont_label[32];
        sprintf(begin_label, ".L.begin.%d", c);
        sprintf(end_label, ".L.end.%d", c);
        sprintf(cont_label, ".L.continue.%d", c);
        cg_def_label(begin_label); // cmp %s, #0
        break_stack[ctrl_depth] = c;
        continue_stack[ctrl_depth] = c;
        ctrl_depth++;
        int r_then = gen(node->then);
        if (r_then != -1) free_reg(r_then);
        cg_def_label(cont_label); // b.ne .L.begin.%d
        int r = gen(node->cond);
#ifdef ARCH_ARM64
        asm_cmp_zero(cg_sec, r, node->cond->ty->size); // cmp $0, %s
        free_reg(r);
        size_t do_j = asm_jcc_label(cg_sec, ARM64_NE); // jne .L.begin.%d
        asm_fixup_add(cg_sec, do_j, begin_label, 1); // .L.end.%d:
#else
        asm_cmp_zero(cg_sec, r, node->cond->ty->size); // %s:
        free_reg(r);
        size_t do_j = asm_jcc_label(cg_sec, X86_NE); // %s:
        asm_fixup_add(cg_sec, do_j, begin_label, 1); // b %s
#endif
        cg_def_label(end_label); // jmp %s
        ctrl_depth--;
        return -1;
    }
    case ND_SWITCH: {
        int c = ++rcc_label_count;
        int cond = gen(node->cond);
        int sz = op_size(node->cond->ty);
        bool is_uns = node->cond->ty && node->cond->ty->is_unsigned;
        for (Node *cs = node->case_next; cs; cs = cs->case_next) {
            if (!cs->label_id)
                cs->label_id = ++rcc_label_count;
            if (cs->is_case_range) {
                int skip_lbl = ++rcc_label_count;
#ifdef ARCH_ARM64
                if ((cs->case_val >= 0 && cs->case_val <= 4095) ||
                    (cs->case_val > 0 && cs->case_val <= 0xffffff && (cs->case_val % 4096) == 0))
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_val); // cmp %s, #%lld
                else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_val);
                    asm_cmp_reg_reg(cg_sec, cond, tmp, 8); // cmp %s, %s
                    free_reg(tmp);
                }
                {
                    size_t o = asm_jcc_label(cg_sec, is_uns ? ARM64_LO : ARM64_LT); // %s .L.skip.%d\n", is_uns ? "b.lo" : "b.lt
                    asm_fixup_add(cg_sec, o, format(".L.skip.%d", skip_lbl), 1);
                } /* cmp %s, #%lld\n */
                if ((cs->case_end >= 0 && cs->case_end <= 4095) ||
                    (cs->case_end > 0 && cs->case_end <= 0xffffff && (cs->case_end % 4096) == 0))
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_end); // cmp %s, %s
                else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_end);
                    asm_cmp_reg_reg(cg_sec, cond, tmp, 8); // %s .L.case.%d\n", is_uns ? "b.ls" : "b.le
                    free_reg(tmp);
                }
                {
                    size_t o = asm_jcc_label(cg_sec, is_uns ? ARM64_LS : ARM64_LE); // cmp $%lld, %s
                    asm_fixup_add(cg_sec, o, format(".L.case.%d", (int)cs->label_id), 1);
                } /* movabs $%lld, %s\n */
#else
                if (cs->case_val == (int32_t)cs->case_val)
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_val); // cmp %s, %s
                else {
                    int tmp = alloc_reg();
                    asm_mov_imm(cg_sec, tmp, 8, (long long)cs->case_val); // %s .L.skip.%d\n", is_uns ? "jb" : "jl
                    asm_cmp_reg_reg(cg_sec, tmp, cond, 8); // cmp $%lld, %s
                    free_reg(tmp);
                }
                {
                    size_t o = asm_jcc_label(cg_sec, is_uns ? X86_B : X86_L); // movabs $%lld, %s
                    asm_fixup_add(cg_sec, o, format(".L.skip.%d", skip_lbl), 1);
                } /* cmp %s, %s\n */
                if (cs->case_end == (int32_t)cs->case_end)
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_end); // %s .L.case.%d\n", is_uns ? "jbe" : "jle
                else {
                    int tmp = alloc_reg();
                    asm_mov_imm(cg_sec, tmp, 8, (long long)cs->case_end); // .L.skip.%d:
                    asm_cmp_reg_reg(cg_sec, tmp, cond, 8); // cmp %s, #%lld
                    free_reg(tmp);
                }
                {
                    size_t o = asm_jcc_label(cg_sec, is_uns ? X86_BE : X86_LE); // cmp %s, %s
                    asm_fixup_add(cg_sec, o, format(".L.case.%d", (int)cs->label_id), 1);
                } /* b.eq .L.case.%d\n */
#endif
                cg_def_label(format(".L.skip.%d", skip_lbl)); // cmp $%lld, %s
            } else {
#ifdef ARCH_ARM64
                if ((cs->case_val >= 0 && cs->case_val <= 4095) ||
                    (cs->case_val > 0 && cs->case_val <= 0xffffff && (cs->case_val % 4096) == 0)) {
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_val); // movabs $%lld, %s
                } else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_val);
                    asm_cmp_reg_reg(cg_sec, cond, tmp, 8); // cmp %s, %s
                    free_reg(tmp);
                }
                asm_jcc_label(cg_sec, ARM64_EQ); // je .L.case.%d
#else
                if (cs->case_val == (int32_t)cs->case_val) {
                    asm_cmp_imm(cg_sec, cond, sz, (long long)cs->case_val); // b .L.case.%d
                } else {
                    int tmp = alloc_reg();
                    asm_mov_imm(cg_sec, tmp, 8, (long long)cs->case_val); // b .L.end.%d
                    asm_cmp_reg_reg(cg_sec, tmp, cond, 8); // jmp .L.case.%d
                    free_reg(tmp);
                }
                size_t case_jmp = asm_jcc_label(cg_sec, X86_E); // jmp .L.end.%d
                asm_fixup_add(cg_sec, case_jmp, format(".L.case.%d", cs->label_id), 1); // cmp %s, #%lld
#endif
            }
        }
        if (node->default_case) {
            if (!node->default_case->label_id)
                node->default_case->label_id = ++rcc_label_count;
            size_t sw_jmp = asm_jmp_label(cg_sec); // cmp %s, %s
            asm_fixup_add(cg_sec, sw_jmp, format(".L.case.%d", node->default_case->label_id), 0); // %s .L.skip.%d\n", is_uns ? "b.lo" : "b.lt
        } else {
            size_t sw_jmp = asm_jmp_label(cg_sec); // cmp %s, #%lld
            asm_fixup_add(cg_sec, sw_jmp, format(".L.end.%d", c), 0); // cmp %s, %s
        }
        free_reg(cond);
        break_stack[ctrl_depth] = c;
        continue_stack[ctrl_depth] = ctrl_depth > 0 ? continue_stack[ctrl_depth - 1] : c;
        ctrl_depth++;
        int r_body = gen(node->then);
        if (r_body != -1) free_reg(r_body);
        ctrl_depth--;
        cg_def_label(format(".L.end.%d", c)); // .L.end.%d:
        return -1;
    }
    case ND_CASE: {
        if (!node->label_id)
            node->label_id = ++rcc_label_count;
        cg_def_label(format(".L.case.%d", node->label_id)); // .L.case.%d:
        return gen(node->lhs);
    }
    case ND_BREAK:
        if (ctrl_depth == 0)
            error("stray break");
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
        {
            size_t brk_off = asm_jmp_label(cg_sec); // b .L.end.%d
            asm_fixup_add(cg_sec, brk_off, format(".L.end.%d", break_stack[ctrl_depth - 1]), 0); // jmp .L.end.%d
        }
        return -1;
    case ND_CONTINUE:
        if (ctrl_depth == 0)
            error("stray continue");
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
        {
            size_t cont_off = asm_jmp_label(cg_sec); // b .L.continue.%d
            asm_fixup_add(cg_sec, cont_off, format(".L.continue.%d", continue_stack[ctrl_depth - 1]), 0); // jmp .L.continue.%d
        }
        return -1;
    case ND_GOTO:
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
        {
            size_t goto_off = asm_jmp_label(cg_sec); // b .L.label.%s.%s
            asm_fixup_add(cg_sec, goto_off, format(".L.label.%s.%s", current_fn, node->label_name), 0); // jmp .L.label.%s.%s
        }
        return -1;
    case ND_GOTO_IND: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        asm_jmp_reg(cg_sec, r); // br %s
#else
        asm_jmp_reg(cg_sec, r); // jmp *%s
#endif
        free_reg(r);
        return -1;
    }
    case ND_LABEL: {
        cg_def_label(format(".L.label.%s.%s", current_fn, node->label_name)); // .L.label.%s.%s:
        return gen(node->lhs);
    }
    case ND_LABEL_VAL: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        emit_adrp_add(reg64[r], format(".L.label.%s.%s", current_fn, node->label_name));
#else
        (void)0 /* FIXME: rip-relative */;
#endif
        return r;
    }
    case ND_ASM: {
#ifdef ARCH_ARM64
        // ARM64 extended inline assembly handler.
        //
        // For each operand:
        //   - Memory (m, Q, Ump): gen_addr → asm_str = "[xN]"
        //   - Output-only register (=r, =w, =x, =y, =&r):
        //       gen_addr for store-back, alloc fresh register for asm use
        //   - Read-write register (+r): gen_addr, alloc fresh reg, load value, store back
        //   - Input register (r, w, x): gen value into register
        //   - Matching constraint (digit): same register as referenced output
        //   - Immediate (I,K,L,M,N): gen value, emit as register (immediate in template)
        //   - Zero (Z): use xzr
        //   - Symbol (S): gen address

        int op_regs[MAX_ASM_OPERANDS]; // GPR for asm template operand (-1 if unused/FP)
        int op_addr[MAX_ASM_OPERANDS]; // Address register for store-back (-1 if none)
        int op_is_fp[MAX_ASM_OPERANDS]; // FP constraint (use d0 as output)
        for (int i = 0; i < node->asm_noperands; i++) {
            op_regs[i] = -1;
            op_addr[i] = -1;
            op_is_fp[i] = 0;
        }

        for (int i = 0; i < node->asm_noperands; i++) {
            AsmOperand *op = &node->asm_ops[i];
            const char *c = op->constraint;
            // skip leading modifiers =, +, &
            while (*c == '=' || *c == '+' || *c == '&') c++;

            // Matching constraint: digit refers to another operand
            if (*c >= '0' && *c <= '9') {
                // defer: will resolve after all others are evaluated
                op_regs[i] = -2; // sentinel: matching
                continue;
            }

            // Determine constraint class
            bool is_mem = op->is_memory || *c == 'Q' ||
                (*c == 'U' && c[1] == 'm' && c[2] == 'p');
            bool is_fp = (*c == 'w' || *c == 'x' || *c == 'y') && !is_mem;
            bool is_imm = (*c == 'I' || *c == 'K' || *c == 'L' ||
                           *c == 'M' || *c == 'N');
            bool is_zero = (*c == 'Z');

            if (is_mem) {
                int r = gen_addr(op->expr);
                op_regs[i] = r;
                op->reg = r;
                snprintf(op->asm_str, sizeof(op->asm_str), "[%s]", reg64[r]);
            } else if (is_fp) {
                if (op->is_output) {
                    // FP output: get address for store-back, use d0 in template
                    int r = gen_addr(op->expr);
                    op_addr[i] = r;
                    op_is_fp[i] = 1;
                    op->reg = -1;
                    snprintf(op->asm_str, sizeof(op->asm_str), "d0");
                } else {
                    // FP input treated as integer register for now
                    int r = gen(op->expr);
                    op_regs[i] = r;
                    op->reg = r;
                    snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
                }
            } else if (is_zero) {
                // Zero constraint: always xzr, no register allocation needed
                snprintf(op->asm_str, sizeof(op->asm_str), "xzr");
                // op_regs[i] stays -1
            } else if (is_imm) {
                // Immediate constraint (I,K,L,M,N): emit as #value in the template.
                // Try to extract compile-time constant; fall back to register.
                int64_t cval = 0;
                if (try_const_int(op->expr, &cval)) {
                    snprintf(op->asm_str, sizeof(op->asm_str), "#0x%llx",
                             (unsigned long long)(uint64_t)cval);
                    // op_regs[i] stays -1, no register needed
                } else {
                    int r = gen(op->expr);
                    op_regs[i] = r;
                    op->reg = r;
                    snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
                }
            } else if (op->is_output && !op->is_rw) {
                // Output-only register (=r, =&r, etc.):
                // Allocate a fresh x register for the asm, store back after.
                // Always use x (64-bit) register in the template; %wN modifier handles 32-bit.
                int r_addr = gen_addr(op->expr);
                op_addr[i] = r_addr;
                int r = alloc_reg();
                op_regs[i] = r;
                op->reg = r;
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
            } else if (op->is_rw) {
                // Read-write register (+r):
                // Allocate fresh x register, load current value, store back after.
                int r_addr = gen_addr(op->expr);
                op_addr[i] = r_addr;
                int r = alloc_reg();
                op_regs[i] = r;
                op->reg = r;
                emit_load(op->expr->ty, r, r_addr, 0);
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
            } else {
                // Input register (r) or symbol address (S):
                int r = gen(op->expr);
                op_regs[i] = r;
                op->reg = r;
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
            }
        }

        // Resolve matching constraints (digit): share register with referenced output.
        // Also load the input value into the shared register before the asm.
        for (int i = 0; i < node->asm_noperands; i++) {
            if (op_regs[i] != -2) continue;
            AsmOperand *op = &node->asm_ops[i];
            const char *c = op->constraint;
            while (*c == '=' || *c == '+' || *c == '&') c++;
            int ref = *c - '0';
            if (ref >= 0 && ref < node->asm_noperands) {
                int r = op_regs[ref];
                // For output operands that compute an address (gen_addr), the
                // matching input needs a SEPARATE value register, not the address reg.
                bool ref_is_addr = node->asm_ops[ref].is_output && !node->asm_ops[ref].is_rw;
                if (ref_is_addr) {
                    // Allocate a fresh register for the input value,
                    // then move it into the output register so the asm
                    // finds it in the shared register.
                    int r_in = gen(op->expr);
                    asm_mov_reg_reg(cg_sec, r, r_in, 8); // mov %s, %s
                    free_reg(r_in);
                    op_regs[i] = r;
                    op->reg = r;
                    snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg64[r]);
                } else {
                    op_regs[i] = r;
                    op->reg = r;
                    strncpy(op->asm_str, node->asm_ops[ref].asm_str, sizeof(op->asm_str) - 1);
                    op->asm_str[sizeof(op->asm_str) - 1] = '\0';
                    if (r >= 0) {
                        int r_in = gen(op->expr);
                        if (r_in != r)
                            asm_mov_reg_reg(cg_sec, r, r_in, 8); // mov %s, %s
                        free_reg(r_in);
                    }
                }
            }
        }

        // Validate the asm template (catches invalid mnemonics, ranges, etc.)
        arm64_validate_asm_template(node->asm_template, node->tok);

        // Translate TCC-specific {$}N to ARM64 immediate #N
        char adj[4096];
        int alen = 0;
        const char *tp = node->asm_template;
        while (*tp && alen < (int)sizeof(adj) - 1) {
            if (*tp == '{' && *(tp + 1) == '$' && *(tp + 2) == '}') {
                adj[alen++] = '#';
                tp += 3;
            } else {
                adj[alen++] = *tp++;
            }
        }
        adj[alen] = '\0';

        // Substitute %N, %wN, %xN, %dN, %l[label], %[name] in template
        char out[4096];
        int olen = 0;
        const char *p = adj;
        while (*p && olen < (int)sizeof(out) - 1) {
            if (*p != '%') {
                out[olen++] = *p++;
                continue;
            }
            p++;
            if (*p == '%') {
                out[olen++] = '%';
                p++;
                continue;
            }
            // Check for modifier letter(s): w, x, d, s, l, h
            char mod = 0;
            if (*p == 'w' || *p == 'x' || *p == 'd' || *p == 's' ||
                *p == 'l' || *p == 'h') {
                // peek: if next char is '[' or digit, it's a modifier
                if (*(p + 1) == '[' || (*(p + 1) >= '0' && *(p + 1) <= '9'))
                    mod = *p++;
            }

            if (mod == 'l' && *p == '[') {
                // %l[name] → goto label
                p++;
                const char *end = strchr(p, ']');
                if (!end) {
                    out[olen++] = '%';
                    out[olen++] = 'l';
                    out[olen++] = '[';
                    continue;
                }
                const char *prefix = ".L.label.";
                for (const char *s = prefix; *s && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                for (const char *s = current_fn; *s && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                if (olen < (int)sizeof(out) - 1) out[olen++] = '.';
                for (const char *s = p; s < end && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                p = end + 1;
            } else if (*p == '[') {
                // %[name] → named operand (not a goto label)
                p++;
                const char *end = strchr(p, ']');
                if (!end) {
                    out[olen++] = '%';
                    out[olen++] = '[';
                    continue;
                }
                // find operand with matching name
                char namebuf[32];
                int nlen = (int)(end - p) < 31 ? (int)(end - p) : 31;
                memcpy(namebuf, p, nlen);
                namebuf[nlen] = '\0';
                p = end + 1;
                const char *sub = NULL;
                for (int j = 0; j < node->asm_noperands; j++) {
                    if (strcmp(node->asm_ops[j].name, namebuf) == 0) {
                        sub = node->asm_ops[j].asm_str;
                        break;
                    }
                }
                if (!sub) sub = "";
                // Apply modifier to named operand
                if (sub[0] && (mod == 'w' || mod == 'x') && sub[0] == 'x') {
                    if (mod == 'w') {
                        if (olen < (int)sizeof(out) - 1) out[olen++] = 'w';
                        sub++; // skip leading 'x'
                    }
                }
                while (*sub && olen < (int)sizeof(out) - 1) out[olen++] = *sub++;
            } else if (*p >= '0' && *p <= '9') {
                int n = *p - '0';
                p++;
                if (n < node->asm_noperands) {
                    const char *s = node->asm_ops[n].asm_str;
                    // Apply modifier
                    if (mod == 'w' && s[0] == 'x') {
                        if (olen < (int)sizeof(out) - 1) out[olen++] = 'w';
                        s++; // skip 'x'
                    } else if (mod == 'd') {
                        // FP double form: dN where N is the reg number
                        if (op_is_fp[n]) {
                            // already stored as "d0" etc
                        } else {
                            if (olen < (int)sizeof(out) - 1) out[olen++] = 'd';
                            // s points to the register name like "x5" → "d5"
                            if (*s == 'x' || *s == 'w') s++;
                            while (*s && olen < (int)sizeof(out) - 1) out[olen++] = *s++;
                            goto next_char;
                        }
                    }
                    while (*s && olen < (int)sizeof(out) - 1) out[olen++] = *s++;
                }
            } else {
                out[olen++] = '%';
                if (mod) out[olen++] = mod;
                // pass through the character that wasn't a modifier/operand
                if (*p && *p != '%') out[olen++] = *p++;
            }
        next_char:;
        }
        out[olen] = '\0';
        if (olen > 0) {
            secbuf_emitbuf(cg_sec, out, olen); // %s
            secbuf_emit8(cg_sec, '\n'); // %s
        }

        // Store back output register operands to their C variables
        for (int i = 0; i < node->asm_noperands; i++) {
            AsmOperand *op = &node->asm_ops[i];
            if (op_addr[i] < 0) continue;
            if (op_is_fp[i]) {
                // Store FP result (d0) back to variable
                int sz = op->expr->ty ? op->expr->ty->size : 8;
                if (sz <= 4)
                    asm_str_fp(cg_sec, 0, op_addr[i], 4); // str s0, [%s]
                else
                    asm_str_fp(cg_sec, 0, op_addr[i], 8); // str d0, [%s]
            } else {
                emit_store(op->expr->ty, op_regs[i], format("[%s]", reg64[op_addr[i]]));
            }
        }

        // Free registers: value regs first, then address regs
        for (int i = 0; i < node->asm_noperands; i++)
            if (op_regs[i] >= 0) free_reg(op_regs[i]);
        for (int i = 0; i < node->asm_noperands; i++)
            if (op_addr[i] >= 0) free_reg(op_addr[i]);
        return -1;
#else
        // x86-64 inline asm operand evaluation.
        // op_regs[i]: register holding value (or address for memory operands)
        // op_addr[i]: address register for store-back after asm (-1 if none)
        int op_regs[MAX_ASM_OPERANDS];
        int op_addr[MAX_ASM_OPERANDS];
        for (int i = 0; i < node->asm_noperands; i++) {
            op_regs[i] = -1;
            op_addr[i] = -1;
        }

        // First pass: allocate registers for outputs and memory operands.
        // Defer matching input constraints (digit) to second pass.
        for (int i = 0; i < node->asm_noperands; i++) {
            AsmOperand *op = &node->asm_ops[i];
            const char *c = op->constraint;
            while (*c == '=' || *c == '+' || *c == '&') c++;

            if (op->is_memory) {
                // "m" constraint: compute address, use as memory ref in template
                int r = gen_addr(op->expr);
                op_regs[i] = r;
                op->reg = r;
                snprintf(op->asm_str, sizeof(op->asm_str), "(%s)", reg64[r]);
            } else if (op->is_output && !op->is_rw) {
                // Output-only: "=r" → allocate fresh register, store back after asm.
                // "=m" is handled above (is_memory).
                int r_addr = gen_addr(op->expr);
                op_addr[i] = r_addr;
                int r = alloc_reg();
                op_regs[i] = r;
                op->reg = r;
                int sz = op->expr->ty ? op->expr->ty->size : 4;
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg(r, sz));
            } else if (op->is_rw) {
                // Read-write "+r": load current value, store back after asm.
                int r_addr = gen_addr(op->expr);
                op_addr[i] = r_addr;
                int r = alloc_reg();
                op_regs[i] = r;
                op->reg = r;
                int sz = op->expr->ty ? op->expr->ty->size : 4;
                emit_load(op->expr->ty, r, r_addr, 0);
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg(r, sz));
            } else if (*c >= '0' && *c <= '9') {
                // Matching constraint: defer to second pass
                op_regs[i] = -2; // sentinel
            } else {
                // Regular input "r": load value into a register
                int r = gen(op->expr);
                op_regs[i] = r;
                op->reg = r;
                int sz = op->expr->ty ? op->expr->ty->size : 4;
                snprintf(op->asm_str, sizeof(op->asm_str), "%s", reg(r, sz));
            }
        }

        // Second pass: resolve matching constraints ("0".."9").
        // Load the input value into the same register as the referenced output.
        for (int i = 0; i < node->asm_noperands; i++) {
            if (op_regs[i] != -2) continue;
            AsmOperand *op = &node->asm_ops[i];
            const char *c = op->constraint;
            while (*c == '=' || *c == '+' || *c == '&') c++;
            int ref = *c - '0';
            if (ref >= 0 && ref < node->asm_noperands && op_regs[ref] >= 0) {
                int r = op_regs[ref];
                // Load the input value into the shared register
                int r_in = gen(op->expr);
                if (r_in != r) {
                    int sz = op->expr->ty ? op->expr->ty->size : 4;
                    if (sz <= 4)
                        asm_mov_reg_reg(cg_sec, r_in, r, 4); // movl %s, %s
                    else
                        asm_mov_reg_reg(cg_sec, r_in, r, 8); // movq %s, %s
                }
                free_reg(r_in);
                op_regs[i] = r;
                op->reg = r;
                strncpy(op->asm_str, node->asm_ops[ref].asm_str, sizeof(op->asm_str) - 1);
                op->asm_str[sizeof(op->asm_str) - 1] = '\0';
            }
        }

        // Emit template with operand substitution, wrapped in AT&T syntax
        /* inline asm already in AT&T */

        // Translate TCC-specific {$}N to GAS-immediate $N in AT&T syntax
        char adj[4096];
        int alen = 0;
        const char *tp = node->asm_template;
        while (*tp && alen < (int)sizeof(adj) - 1) {
            if (*tp == '{' && *(tp + 1) == '$' && *(tp + 2) == '}') {
                adj[alen++] = '$';
                tp += 3;
            } else {
                adj[alen++] = *tp++;
            }
        }
        adj[alen] = '\0';

        // Substitute %N and %l[name] in template
        char out[4096];
        int olen = 0;
        const char *p = adj;
        while (*p && olen < (int)sizeof(out) - 1) {
            if (*p != '%') {
                out[olen++] = *p++;
                continue;
            }
            p++;
            if (*p == '%') {
                out[olen++] = '%';
                p++;
                continue;
            }
            // check for modifier 'l'
            char mod = 0;
            if (*p == 'l') { mod = *p++; }
            if (mod == 'l' && *p == '[') {
                // %l[name] -> goto label
                p++;
                const char *end = strchr(p, ']');
                if (!end) {
                    out[olen++] = '%';
                    out[olen++] = 'l';
                    out[olen++] = '[';
                    continue;
                }
                // emit .L.label.<fn>.<name>
                const char *prefix = ".L.label.";
                for (const char *s = prefix; *s && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                for (const char *s = current_fn; *s && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                if (olen < (int)sizeof(out) - 1) out[olen++] = '.';
                for (const char *s = p; s < end && olen < (int)sizeof(out) - 1;) out[olen++] = *s++;
                p = end + 1;
            } else if (*p >= '0' && *p <= '9') {
                int n = *p - '0';
                p++;
                if (n < node->asm_noperands) {
                    const char *s = node->asm_ops[n].asm_str;
                    while (*s && olen < (int)sizeof(out) - 1) out[olen++] = *s++;
                }
            } else {
                out[olen++] = '%';
                if (mod) out[olen++] = mod;
                // leave other %x as-is
            }
        }
        out[olen] = '\0';
        if (olen > 0) {
            secbuf_emitbuf(cg_sec, out, olen); // %s
            secbuf_emit8(cg_sec, '\n'); // %s
        }

        // Store back register outputs ("=r", "+r") to their C variables
        for (int i = 0; i < node->asm_noperands; i++) {
            if (op_addr[i] < 0) continue;
            AsmOperand *op = &node->asm_ops[i];
            int sz = op->expr->ty ? op->expr->ty->size : 4;
            if (sz == 1)
                (void)0 /* FIXME: sized mov */;
            else if (sz == 2)
                (void)0 /* FIXME: sized mov */;
            else if (sz <= 4)
                x86_mov_mr(cg_sec, 4, x86_mem(CG_X86_REG(op_addr[i]), 0), CG_X86_REG(op_regs[i])); // movb %s, (%s)
            else
                x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(op_addr[i]), 0), CG_X86_REG(op_regs[i])); // movw %s, (%s)
        }

        // Free registers
        for (int i = 0; i < node->asm_noperands; i++)
            if (op_regs[i] >= 0) free_reg(op_regs[i]);
        for (int i = 0; i < node->asm_noperands; i++)
            if (op_addr[i] >= 0) free_reg(op_addr[i]);
        return -1;
#endif
    }
    case ND_VA_START: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        // AArch64 ABI va_list: [__stack(8), __gr_top(8), __vr_top(8), __gr_offs(4), __vr_offs(4)]
        // __stack: pointer to first stack overflow argument
        (void)0 /* FIXME: add phy */;
        (void)0 /* FIXME: ldr/str phy/off */;
        // __gr_top: end of GP reg save area = saved_sp + 64
        (void)0 /* FIXME: sized ld/st */;
        (void)0 /* FIXME: add phy */;
        (void)0 /* FIXME: ldr/str phy/off */;
        // __vr_top: end of FP reg save area = saved_sp + 192
        (void)0 /* FIXME: sized ld/st */;
        (void)0 /* FIXME: add phy */;
        (void)0 /* FIXME: ldr/str phy/off */;
        // __gr_offs: -(8 - gp_param) * 8
        asm_mov_imm(cg_sec, 16, 4, va_gp_start); // add x16, %s, #%d
        (void)0 /* FIXME: ldr/str phy/off */;
        // __vr_offs: -(8 - fp_param) * 16
        asm_mov_imm(cg_sec, 16, 4, va_fp_start); // str x16, [%s]
        (void)0 /* FIXME: ldr/str phy/off */;
#else
        (void)0 /* FIXME: movl imm */;
        (void)0 /* FIXME: movl imm */;
        (void)0 /* FIXME: lea */;
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(r), 8), X86_RDX); // ldur x16, [%s, #-8]
        asm_lea_rbp_phy(cg_sec, X86_RDX, 8, va_reg_save_ofs); // add x16, x16, #64
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(r), 16), X86_RDX); // str x16, [%s, #8]
#endif
        free_reg(r);
        return -1;
    }
    case ND_VA_COPY: {
        int rd = gen(node->lhs);
#ifdef ARCH_ARM64
        int rs = gen(node->rhs);
        // ABI va_list is 32 bytes: copy all 4 x 8-byte words
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        (void)0 /* FIXME: ldr/str phy/off */;
        free_reg(rd);
        free_reg(rs);
#else
        x86_push(cg_sec, CG_X86_REG(rd)); // ldr x16, [%s]
        free_reg(rd);
        int rs = gen(node->rhs);
        int rpop = alloc_reg();
        x86_pop(cg_sec, CG_X86_REG(rpop)); // str x16, [%s]
        asm_mov_reg_reg(cg_sec, 1, rs, 8); // ldr x16, [%s, #8]
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(rpop), 0), X86_RCX); // str x16, [%s, #8]
        asm_mov_reg_reg(cg_sec, 1, rs, 8); // ldr x16, [%s, #16]
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(rpop), 8), X86_RCX); // str x16, [%s, #16]
        asm_mov_reg_reg(cg_sec, 1, rs, 8); // ldr x16, [%s, #24]
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(rpop), 16), X86_RCX); // str x16, [%s, #24]
        free_reg(rs);
        free_reg(rpop);
#endif
        return -1;
    }
    case ND_VA_ARG: {
        int r = gen(node->lhs);
        Type *ty = node->ty->base;
        bool is_fp = is_flonum(ty);
#ifdef ARCH_ARM64
        bool is_ptr_struct = (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->size > 16;
        // rcc passes ALL structs >8 bytes by pointer (not by value).
        // va_arg must read 8-byte pointer from reg save area, then dereference.
        bool is_ptr_val_struct = (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->size > 8;

        // AArch64 ABI va_list: [__stack(8), __gr_top(8), __vr_top(8), __gr_offs(4), __vr_offs(4)]
        // gr_offs < 0 means reg arg available; addr = __gr_top + __gr_offs
        // Only use FP path for bare float types; structs (incl. HFA) always go in GP regs.
        if (is_fp) {
            int fp_size = 16;
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_cmp_imm(cg_sec, 16, 8 if 0 == "1" else 4, 0); // ldr w16, [%s, #28]
            asm_jcc_label(cg_sec, ARM64_GE); // cmp w16, #0
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_sxtw(cg_sec, 17, 16); // b.ge .L.va_overflow.%d
            (void)0 /* FIXME: add phy */;
            (void)0 /* FIXME: add phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_jmp_label(cg_sec); // ldr x12, [%s, #16]
        } else {
            // Pointer-passed structs use gp_size=8 (the pointer fits in one register);
            // after reading from the reg save area we must dereference it.
            // Normal types use 8 bytes (fits in one register).
            int gp_size = 8;
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_cmp_imm(cg_sec, 16, 8 if 0 == "1" else 4, 0); // sxtw x17, w16
            asm_jcc_label(cg_sec, ARM64_GE); // add x12, x12, x17
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_sxtw(cg_sec, 17, 16); // add w16, w16, #%d
            (void)0 /* FIXME: add phy */;
            if (is_ptr_val_struct) {
                (void)0 /* FIXME: ldr/str phy/off */;
            }
            (void)0 /* FIXME: add phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
            asm_jmp_label(cg_sec); // str w16, [%s, #28]
        }

        (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
        (void)0 /* FIXME: ldr/str phy/off */;
        if (is_ptr_struct || is_ptr_val_struct) {
            // Pointer-passed struct: load pointer from stack, advance by 8
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: add phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
            (void)0 /* FIXME: mov phy */;
        } else {
            int align = ty->align;
            int ovf_size = ty->size <= 8 ? 8 : (ty->size + 7) & ~7;
            if (align > 8) {
                (void)0 /* FIXME: add phy */;
                (void)0 /* FIXME: and variant */;
            }
            (void)0 /* FIXME: mov phy */;
            (void)0 /* FIXME: add phy */;
            (void)0 /* FIXME: ldr/str phy/off */;
        }

        (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
        (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
        asm_mov_reg_reg(cg_sec, r, 2, 8); // b .L.va_done.%d
#else
        bool is_ptr_struct = (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->size > 8;
        if (is_fp) {
            (void)0 /* FIXME: cmp variant */;
            asm_jcc_label(cg_sec, X86_A); // ldr w16, [%s, #24]
            x86_mov_rm(cg_sec, 4, X86_RCX, x86_mem(CG_X86_REG(r), 4)); // cmp w16, #0
            (void)0 /* FIXME: sized alu op */;
            (void)0 /* FIXME: sized alu op */;
            asm_jmp_label(cg_sec); // b.ge .L.va_overflow.%d
        } else if (is_ptr_struct) {
            (void)0 /* FIXME: cmp variant */;
            asm_jcc_label(cg_sec, X86_A); // ldr x12, [%s, #8]
            (void)0 /* FIXME: indirect mov */;
            (void)0 /* FIXME: sized alu op */;
            (void)0 /* FIXME: indirect mov */;
            (void)0 /* FIXME: sized alu op */;
            asm_jmp_label(cg_sec); // sxtw x17, w16
        } else {
            (void)0 /* FIXME: cmp variant */;
            asm_jcc_label(cg_sec, X86_A); // add x12, x12, x17
            (void)0 /* FIXME: indirect mov */;
            (void)0 /* FIXME: sized alu op */;
            (void)0 /* FIXME: sized alu op */;
            asm_jmp_label(cg_sec); // ldr x12, [x12]
        }
        (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
        asm_mov_reg_reg(cg_sec, 1, r, 8); // add w16, w16, #%d
        (void)0 /* FIXME: lea */;
        x86_mov_mr(cg_sec, 8, x86_mem(CG_X86_REG(r), 8), X86_RDX); // str w16, [%s, #24]
        if (is_ptr_struct)
            (void)0 /* FIXME: indirect mov */;
        (void)0 /* FIXME: label .L.xxx.rcc_label_count */;
        x86_mov_rr(cg_sec, 8, CG_X86_REG(r), X86_RCX); // b .L.va_done.%d
#endif
        rcc_label_count++;
        return r;
    }

    case ND_ATOMIC_LOAD: {
        int r_addr = gen(node->lhs);
        int sz = node->ty->size;
        int r = alloc_reg();
        int ord = node->atomic_ord;
#ifdef ARCH_ARM64
        char *sz_suffix = (sz == 1) ? "b" : (sz == 2) ? "h"
                                                      : "";
        bool use_acquire = (ord == MEMORDER_ACQUIRE || ord == MEMORDER_ACQ_REL || ord == MEMORDER_SEQ_CST || ord == MEMORDER_CONSUME);
        if (use_acquire) {
            if (sz == 1) asm_ldarb(cg_sec, r, r_addr)); // ldar%s %s, [%s]
            else if (sz == 2) asm_ldarh(cg_sec, r, r_addr)); // ldar%s %s, [%s]
            else
                secbuf_emit32le(cg_sec, arm64_ldar(sz == 8 ? 1 : 0, CG_ARM_REG(r), CG_ARM_REG(r_addr)));
        } else
            emit_load(node->ty, r, r_addr, 0);
#else
        if (sz < 4) {
            X86Mem m = {CG_X86_REG(r_addr), X86_NOREG, 1, 0};
            if (sz == 1) {
                if (use_unsigned(node->ty))
                    x86_movzx_rm(cg_sec, 4, 1, CG_X86_REG(r), m);
                else
                    x86_movsx_rm(cg_sec, 4, 1, CG_X86_REG(r), m);
            } else {
                if (use_unsigned(node->ty))
                    x86_movzx_rm(cg_sec, 4, 2, CG_X86_REG(r), m);
                else
                    x86_movsx_rm(cg_sec, 4, 2, CG_X86_REG(r), m);
            }
        } else if (sz == 4) {
            (void)0 /* FIXME: indirect mov */;
        } else {
            (void)0 /* FIXME: indirect mov */;
        }
        if (ord == MEMORDER_SEQ_CST)
            (void)0 /* mfence TODO */;
#endif
        free_reg(r_addr);
#ifdef ARCH_ARM64
        if (use_acquire && sz < 8 && !use_unsigned(node->ty))
            sign_extend_to(r, sz, 8);
#endif
        return r;
    }
    case ND_ATOMIC_STORE: {
        int r_addr = gen(node->lhs);
        int r_val = gen(node->rhs);
        int sz = node->lhs->ty && node->lhs->ty->base ? node->lhs->ty->base->size : 4;
        if (sz < 1 || sz > 8) sz = 4;
        int ord = node->atomic_ord;
#ifdef ARCH_ARM64
        char *sz_suffix = (sz == 1) ? "b" : (sz == 2) ? "h"
                                                      : "";
        bool use_release = (ord == MEMORDER_RELEASE || ord == MEMORDER_ACQ_REL || ord == MEMORDER_SEQ_CST);
        if (use_release) {
            if (sz == 1) asm_stlrb(cg_sec, r_val, r_addr)); // stlr%s %s, [%s]
            else if (sz == 2) asm_stlrh(cg_sec, r_val, r_addr)); // dmb ish
            else
                secbuf_emit32le(cg_sec, arm64_stlr(sz == 8 ? 1 : 0, CG_ARM_REG(r_val), CG_ARM_REG(r_addr))); // mov%c %s, (%s)
        } else
            emit_store(node->lhs->ty->base ? node->lhs->ty->base : ty_int, r_val, r_addr, 0);
        if (ord == MEMORDER_SEQ_CST)
            asm_dmb(cg_sec); // mfence
#else
        (void)0 /* FIXME: sized mov */;
        if (ord == MEMORDER_SEQ_CST)
            (void)0 /* mfence TODO */;
#endif
        free_reg(r_val);
        free_reg(r_addr);
        return -1;
    }
    case ND_ATOMIC_EXCHANGE: {
        int r_addr = gen(node->lhs);
        int r_val = gen(node->rhs);
        int sz = node->ty->size;
        int r_result = alloc_reg();
#ifdef ARCH_ARM64
        int lbl = rcc_label_count++;
        (void)0 /* FIXME: label .L.xxx.lbl */;
        if (sz == 1) {
            (void)0 /* FIXME: atomic */;
            (void)0 /* FIXME: atomic */;
        } else if (sz == 2) {
            (void)0 /* FIXME: atomic */;
            (void)0 /* FIXME: atomic */;
        } else if (sz == 4) {
            (void)0 /* FIXME: atomic */;
            (void)0 /* FIXME: atomic */;
        } else {
            (void)0 /* FIXME: atomic */;
            (void)0 /* FIXME: atomic */;
        }
        asm_jcc_label(cg_sec, ARM64_NE); // .L.atom_xchg.%d:
#else
        (void)0 /* FIXME: xchg */;
        if (sz < 4) {
            asm_mov_reg_reg(cg_sec, r_val, r_result, 8); // ldxrb %s, [%s]
            if (use_unsigned(node->ty))
                zero_extend_to(r_result, sz, 4);
            else
                sign_extend_to(r_result, sz, 4);
        } else {
            asm_mov_reg_reg(cg_sec, r_val, r_result, 8); // stxrb w9, %s, [%s]
        }
#endif
        free_reg(r_val);
        free_reg(r_addr);
#ifdef ARCH_ARM64
        if (sz < 8 && !use_unsigned(node->ty))
            sign_extend_to(r_result, sz, 8);
#endif
        return r_result;
    }
    case ND_ATOMIC_CAS: {
        int r_addr = gen(node->lhs);
        int r_expectedaddr = gen(node->body);
        int r_desired = gen(node->rhs);
        int sz = node->lhs->ty && node->lhs->ty->base ? node->lhs->ty->base->size : 4;
        int r_result = alloc_reg();
#ifdef ARCH_ARM64
        int r_expected = alloc_reg();
        Type *elem_ty = node->lhs->ty && node->lhs->ty->base ? node->lhs->ty->base : ty_int;
        emit_load(elem_ty, r_expected, r_expectedaddr, 0);
        int r_old = alloc_reg();
        int lbl = rcc_label_count++;
        (void)0 /* FIXME: label .L.xxx.lbl */;
        if (sz == 1)
            (void)0 /* FIXME: atomic */;
        else if (sz == 2)
            (void)0 /* FIXME: atomic */;
        else if (sz == 4)
            (void)0 /* FIXME: atomic */;
        else
            (void)0 /* FIXME: atomic */;
        asm_cmp_reg_reg(cg_sec, r_old, r_expected, 8); // .L.atom_cas.%d:
        asm_jcc_label(cg_sec, ARM64_NE); // ldxrb %s, [%s]
        if (sz == 1)
            (void)0 /* FIXME: atomic */;
        else if (sz == 2)
            (void)0 /* FIXME: atomic */;
        else if (sz == 4)
            (void)0 /* FIXME: atomic */;
        else
            (void)0 /* FIXME: atomic */;
        if (node->atomic_weak)
            asm_jcc_label(cg_sec, ARM64_NE); // ldxrh %s, [%s]
        else
            asm_jcc_label(cg_sec, ARM64_NE); // ldxr %s, [%s]
        asm_mov_imm(cg_sec, r_result, 8, 1); // ldxr %s, [%s]
        asm_jmp_label(cg_sec); // cmp %s, %s
        (void)0 /* FIXME: label .L.xxx.lbl */;
        asm_movq_zero(cg_sec, r_result); // b.ne .L.atom_cas_fail.%d
        emit_store(elem_ty, r_old, r_expectedaddr, 0);
        (void)0 /* FIXME: label .L.xxx.lbl */;
        free_reg(r_old);
        free_reg(r_expected);
#else
        int r_expected = alloc_reg();
        if (sz == 1)
            (void)0 /* FIXME: indirect ext */;
        else if (sz == 2)
            (void)0 /* FIXME: indirect ext */;
        else if (sz == 4)
            (void)0 /* FIXME: indirect mov */;
        else
            (void)0 /* FIXME: indirect mov */;
        (void)0 /* FIXME: lock */;
        (void)0 /* FIXME: sete */;
        asm_movzx(cg_sec, r_result, r_result, 4, 1); // stxrb w9, %s, [%s]
        x86_mov_mr(cg_sec, sz, x86_mem(CG_X86_REG(r_expectedaddr), 0), X86_RAX);
        free_reg(r_expected);
#endif
        free_reg(r_desired);
        free_reg(r_expectedaddr);
        free_reg(r_addr);
        return r_result;
    }
    case ND_ATOMIC_FENCE: {
        int ord = node->atomic_ord;
        if (!node->atomic_signal_fence) {
            if (ord == MEMORDER_SEQ_CST || ord == MEMORDER_ACQ_REL) {
#ifdef ARCH_ARM64
                asm_dmb(cg_sec); // dmb ish
#else
                (void)0 /* mfence TODO */;
#endif
            } else if (ord == MEMORDER_ACQUIRE || ord == MEMORDER_CONSUME) {
#ifdef ARCH_ARM64
                asm_dmb(cg_sec); // mfence
#endif
            } else if (ord == MEMORDER_RELEASE) {
#ifdef ARCH_ARM64
                asm_dmb(cg_sec); // dmb ishld
#endif
            }
        }
        return -1;
    }
    case ND_ATOMIC_FETCH_OP: {
        int r_addr = gen(node->lhs);
        int r_val = gen(node->rhs);
        int sz = node->ty->size;
        int op = node->atomic_fetch_op;
        bool is_store = node->atomic_is_store;
#ifdef ARCH_ARM64
        int r_value = alloc_reg();
        asm_mov_reg_reg(cg_sec, r_value, r_val, 8); // mov %s, %s\n", sz == 8 ? "x9" : "w9
        free_reg(r_val);
        int old_dummy = alloc_reg();
        int old_slot = spill_offset(old_dummy);
        free_reg(old_dummy);
        int r_tmp = alloc_reg();
        int lbl = rcc_label_count++;
        (void)0 /* FIXME: label .L.xxx.lbl */;
        if (sz == 1)
            (void)0 /* FIXME: atomic */;
        else if (sz == 2)
            (void)0 /* FIXME: atomic */;
        else if (sz == 4)
            (void)0 /* FIXME: atomic */;
        else
            (void)0 /* FIXME: atomic */;
        asm_str_fp_imm(cg_sec, r_tmp, 8, old_slot); // .L.atom_fop.%d:
        int sf = (sz == 8) ? 1 : 0;
        switch (op) {
        case 0: asm_add_reg_reg(cg_sec, r_tmp, 9, sz); break; // add r_tmp, r_tmp, r9
        case 1: asm_sub_reg_reg(cg_sec, r_tmp, 9, sz); break; // sub r_tmp, r_tmp, r9
        case 2: asm_or_reg_reg(cg_sec, r_tmp, 9, sz); break;  // orr r_tmp, r_tmp, r9
        case 3: asm_eor_reg_reg(cg_sec, r_tmp, 9, sz); break; // eor r_tmp, r_tmp, r9
        case 4: asm_and_reg_reg(cg_sec, r_tmp, 9, sz); break; // and r_tmp, r_tmp, r9
        case 5:
            asm_and_reg_reg(cg_sec, r_tmp, r_tmp, sz); // and %s, %s, %s
            asm_not(cg_sec, r_tmp, sz); // mvn %s, %s
            break;
        }
        if (sz == 1)
            (void)0 /* FIXME: atomic */;
        else if (sz == 2)
            (void)0 /* FIXME: atomic */;
        else if (sz == 4)
            (void)0 /* FIXME: atomic */;
        else
            (void)0 /* FIXME: atomic */;
        asm_jcc_label(cg_sec, ARM64_NE); // stxrb w8, %s, [%s]
        if (node->atomic_ord == MEMORDER_SEQ_CST)
            asm_dmb(cg_sec); // stxrh w8, %s, [%s]
        if (!is_store)
            asm_ldr_fp_imm(cg_sec, r_tmp, 8, old_slot); // stxr w8, %s, [%s]
        free_reg(r_addr);
        if (sz < 8 && !use_unsigned(node->ty))
            sign_extend_to(r_tmp, sz, 8);
        return r_tmp;
#else
        int r_old = alloc_reg();
        if (op == 0 || op == 1) {
            if (op == 1)
                asm_neg(cg_sec, r_val, sz); // neg %s
            // Save val to stack before xadd clobbers it
            (void)0 /* FIXME: sized mov */;
            asm_mov_reg_reg(cg_sec, r_val, r_old, 8); // mov%c %s, -%d(%%rbp)
            free_reg(r_val);
            if (r_old == r_addr && (spilled_regs & (1 << r_addr))) {
                asm_mov_rbp_reg(cg_sec, r_addr, 8, spill_offset(r_addr)); // mov %s, %s
            }
            (void)0 /* FIXME: lock */;
            free_reg(r_addr);
            if (node->atomic_ord == MEMORDER_SEQ_CST)
                (void)0 /* mfence TODO */;
            if (is_store) {
                // add_fetch/sub_fetch: return new value = old + val
                (void)0 /* FIXME: sized alu op */;
            }
            if (sz < 4) {
                if (!use_unsigned(node->ty))
                    sign_extend_to(r_old, sz, 4);
                else
                    zero_extend_to(r_old, sz, 4);
            }
            return r_old;
        } else {
            // Save r_val to stack before it might be spilled
            (void)0 /* FIXME: sized mov */;
            free_reg(r_val);
            int r_new = alloc_reg();
            int lbl2 = rcc_label_count++;
            (void)0 /* FIXME: label .L.xxx.lbl2 */;
            if (r_new == r_addr && (spilled_regs & (1 << r_addr))) {
                asm_mov_rbp_reg(cg_sec, r_addr, 8, spill_offset(r_addr)); // mov%c %s, -%d(%%rbp)
            } else if (r_old == r_addr && (spilled_regs & (1 << r_addr))) {
                asm_mov_rbp_reg(cg_sec, r_addr, 8, spill_offset(r_addr)); // .L.atom_fop.%d:
            }
            (void)0 /* FIXME: sized mov */;
            // Save old value before computing new (r_old may == r_new)
            (void)0 /* FIXME: sized mov */;
            asm_mov_reg_reg(cg_sec, r_old, r_new, 8); // mov -%d(%%rbp), %s
            char sc = size_suffix(sz);
            switch (op) {
            case 2: asm_or_rbp_reg(cg_sec, r_new, sz, spill_logand); break;
            case 3: asm_xor_rbp_reg(cg_sec, r_new, sz, spill_logand); break;
            case 4: asm_and_rbp_reg(cg_sec, r_new, sz, spill_logand); break;
            case 5:
                (void)0 /* FIXME: sized alu op */;
                asm_not(cg_sec, r_new, sz); // mov -%d(%%rbp), %s
                break;
            }
            (void)0 /* FIXME: lock */;
            asm_jcc_label(cg_sec, X86_NE); // mov%c (%s), %s
            if (node->atomic_ord == MEMORDER_SEQ_CST)
                (void)0 /* mfence TODO */;
            free_reg(r_addr);
            if (is_store) {
                free_reg(r_old);
                if (sz < 4 && !use_unsigned(node->ty))
                    sign_extend_to(r_new, sz, 4);
                else if (sz < 4)
                    zero_extend_to(r_new, sz, 4);
                return r_new;
            } else {
                free_reg(r_new);
                if (r_old == r_new)
                    (void)0 /* FIXME: sized mov */;
                if (sz < 4 && !use_unsigned(node->ty))
                    sign_extend_to(r_old, sz, 4);
                else if (sz < 4)
                    zero_extend_to(r_old, sz, 4);
                return r_old;
            }
        }
#endif
    }

    default:
        break;
    }

    int r_lhs = gen(node->lhs);

    // Float binary operations (must come before integer ops)
    if (is_flonum(node->ty) && (node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_MUL || node->kind == ND_DIV)) {
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        (void)0 /* FIXME: fmov */;
        (void)0 /* FIXME: fmov */;
        char *inst = "";
        if (node->kind == ND_ADD) inst = "fadd";
        else if (node->kind == ND_SUB)
            inst = "fsub";
        else if (node->kind == ND_MUL)
            inst = "fmul";
        else if (node->kind == ND_DIV)
            inst = "fdiv";
        if (node->kind == ND_ADD) asm_fadd(cg_sec, 1); // %s d0, d0, d1
        else if (node->kind == ND_SUB)
            asm_fsub(cg_sec, 1); // fcvt s0, d0
        else if (node->kind == ND_MUL)
            asm_fmul(cg_sec, 1); // fcvt d0, s0
        else if (node->kind == ND_DIV)
            asm_fdiv(cg_sec, 1); // fmov %s, d0
        if (node->ty->kind == TY_FLOAT) {
            asm_fcvt(cg_sec, 3, 0, 0, 0); // movq %s, %%xmm0
            asm_fcvt(cg_sec, 1, 0, 0, 0); // movq %s, %%xmm1
        }
        asm_fmov_f2i(cg_sec, r_lhs, 0, 1); // %s d0, d0, d1
#else
        asm_movq_r_xmm(cg_sec, X86_XMM0, r_lhs); // fcvt s0, d0
        asm_movq_r_xmm(cg_sec, X86_XMM1, r_rhs); // fcvt d0, s0
        free_reg(r_rhs);
        char *inst = "";
        if (node->kind == ND_ADD) inst = "addsd";
        else if (node->kind == ND_SUB)
            inst = "subsd";
        else if (node->kind == ND_MUL)
            inst = "mulsd";
        else if (node->kind == ND_DIV)
            inst = "divsd";
        if (node->kind == ND_ADD) asm_addsd(cg_sec); // %s %%xmm1, %%xmm0
        else if (node->kind == ND_SUB)
            asm_subsd(cg_sec); // cvtsd2ss %%xmm0, %%xmm0
        else if (node->kind == ND_MUL)
            asm_mulsd(cg_sec); // cvtss2sd %%xmm0, %%xmm0
        else if (node->kind == ND_DIV)
            asm_divsd(cg_sec); // movq %%xmm0, %s
        if (node->ty->kind == TY_FLOAT) {
            asm_cvtsd2ss(cg_sec); // %s %%xmm1, %%xmm0
            asm_cvtss2sd(cg_sec); // cvtsd2ss %%xmm0, %%xmm0
        }
        asm_movq_xmm_r(cg_sec, r_lhs, X86_XMM0); // cvtss2sd %%xmm0, %%xmm0
#endif
        free_reg(r_rhs);
        return r_lhs;
    }

    // Float comparisons
    if ((node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) &&
        node->lhs->ty && node->rhs->ty && (is_flonum(node->lhs->ty) || is_flonum(node->rhs->ty))) {
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        (void)0 /* FIXME: fmov */;
        (void)0 /* FIXME: fmov */;
        asm_fcmp(cg_sec, 1); // fmov d0, %s
        if (node->kind == ND_EQ) asm_cset(cg_sec, r_lhs, ARM64_EQ); // fmov d1, %s
        else if (node->kind == ND_NE)
            asm_cset(cg_sec, r_lhs, ARM64_NE); // fcmp d0, d1
        else if (node->kind == ND_LT)
            asm_cset(cg_sec, r_lhs, ARM64_MI); // cset %s, %s
        else if (node->kind == ND_LE)
            asm_cset(cg_sec, r_lhs, ARM64_LS); // movq %s, %%xmm0
#else
        asm_movq_r_xmm(cg_sec, X86_XMM0, r_lhs); // movq %s, %%xmm1
        asm_movq_r_xmm(cg_sec, X86_XMM1, r_rhs); // ucomisd %%xmm1, %%xmm0
        asm_ucomisd(cg_sec); // sete %%al
        if (node->kind == ND_EQ) {
            asm_setcc(cg_sec, 0, X86_E); // setnp %%cl
            asm_setcc(cg_sec, 1, X86_NP); // andb %%cl, %%al
            x86_and_rr(cg_sec, 1, X86_RAX, X86_RCX); // setne %%al
        } else if (node->kind == ND_NE) {
            asm_setcc(cg_sec, 0, X86_NE); // setp %%cl
            asm_setcc(cg_sec, 1, X86_P); // orb %%cl, %%al
            x86_or_rr(cg_sec, 1, X86_RAX, X86_RCX); // setb %%al
        } else if (node->kind == ND_LT) {
            asm_setcc(cg_sec, 0, X86_B); // setbe %%al
        } else if (node->kind == ND_LE) {
            asm_setcc(cg_sec, 0, X86_BE); // movzbl %%al, %s
        }
        asm_movzx(cg_sec, r_lhs, 0, 4, 1); // fmov d0, %s
#endif
        free_reg(r_rhs);
        return r_lhs;
    }

    // Fused Division/Modulo Optimization
    if (node->kind == ND_DIV || node->kind == ND_MOD) {
        int sz = op_size(node->ty);
        bool is_unsigned = use_unsigned(node->ty);
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        int sf = (sz == 8) ? 1 : 0;
        if (node->kind == ND_DIV) {
            secbuf_emit32le(cg_sec, (is_unsigned ? arm64_udiv : arm64_sdiv)(sf, CG_ARM_REG(r_lhs), CG_ARM_REG(r_lhs), CG_ARM_REG(r_rhs))); // %s %s, %s, %s\n", is_unsigned ? "udiv" : "sdiv
        } else {
            int tmp = alloc_reg();
            secbuf_emit32le(cg_sec, (is_unsigned ? arm64_udiv : arm64_sdiv)(sf, CG_ARM_REG(tmp), CG_ARM_REG(r_lhs), CG_ARM_REG(r_rhs))); // %s %s, %s, %s\n", is_unsigned ? "udiv" : "sdiv
            asm_mul_reg_reg(cg_sec, tmp, tmp, sz); // mul %s, %s, %s
            asm_sub_reg_reg(cg_sec, r_lhs, r_lhs, sz); // sub %s, %s, %s
            free_reg(tmp);
        }
#else
        x86_mov_rr(cg_sec, sz, X86_RAX, CG_X86_REG(r_lhs)); // mov %s, %s\n", reg(r_lhs, sz), sz == 8 ? "%rax" : "%eax
        if (is_unsigned) {
            x86_xor_rr(cg_sec, 4, X86_RDX, X86_RDX); // xorl %%edx, %%edx
        } else {
            if (sz == 8) x86_cqo(cg_sec); // xorl %%edx, %%edx
            else
                x86_cdq(cg_sec); // cqo
        }
        if (is_unsigned)
            x86_div_r(cg_sec, sz, CG_X86_REG(r_rhs)); // cdq
        else
            x86_idiv_r(cg_sec, sz, CG_X86_REG(r_rhs)); // %s %s\n", is_unsigned ? "div" : "idiv
        if (node->kind == ND_DIV)
            x86_mov_rr(cg_sec, sz, CG_X86_REG(r_lhs), X86_RAX); // mov %s, %s\n", node->kind == ND_DIV ? (sz == 8 ? "%rax" : "%eax") : (sz == 8 ? "%rdx" : "%edx
        else
            x86_mov_rr(cg_sec, sz, CG_X86_REG(r_lhs), X86_RDX); // %s %s, %s, %s\n", is_unsigned ? "udiv" : "sdiv
#endif
        free_reg(r_rhs);
        return r_lhs;
    }

    // Binary operators with potential immediate/memory optimization for RHS
    if (node->kind == ND_SHL || node->kind == ND_SHR) {
        int sz = op_size(node->ty);
#ifdef ARCH_ARM64
        const char *shift_op = node->kind == ND_SHL ? "lsl" : (use_unsigned(node->ty) ? "lsr" : "asr");
        int sf = (sz == 8) ? 1 : 0;
        if (node->rhs->kind == ND_NUM) {
            int s = (int)node->rhs->val;
            if (s >= sz * 8) {
                asm_movq_zero(cg_sec, r_lhs); // mov %s, #0
            } else {
                if (node->kind == ND_SHL)
                    asm_shl_imm(cg_sec, r_lhs, sf=="1" ? 8 : 4, (uint8_t)(s)); // %s %s, %s, #%d
                else if (use_unsigned(node->ty))
                    asm_shr_imm(cg_sec, r_lhs, sf=="1" ? 8 : 4, (uint8_t)(s)); // %s %s, %s, %s
                else
                    asm_sar_imm(cg_sec, r_lhs, sf=="1" ? 8 : 4, (uint8_t)(s)); // %s $%d, %s\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar
            }
        } else {
            int r_rhs = gen(node->rhs);
            if (node->kind == ND_SHL)
                asm_shl_cl(cg_sec, r_lhs, sf=="1" ? 8 : 4)); // movl %s, %%ecx
            else if (use_unsigned(node->ty))
                asm_shr_cl(cg_sec, r_lhs, sf=="1" ? 8 : 4)); // %s %%cl, %s\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar
            else
                asm_sar_cl(cg_sec, r_lhs, sf=="1" ? 8 : 4)); // mov %s, #0
            free_reg(r_rhs);
        }
#else
        if (node->rhs->kind == ND_NUM) {
            int imm = (int)node->rhs->val;
            if (node->kind == ND_SHL)
                asm_shl_imm(cg_sec, r_lhs, sz, (uint8_t)(imm)); // %s %s, %s, #%d
            else if (use_unsigned(node->ty))
                asm_shr_imm(cg_sec, r_lhs, sz, (uint8_t)(imm)); // %s %s, %s, %s
            else
                asm_sar_imm(cg_sec, r_lhs, sz, (uint8_t)(imm)); // %s $%d, %s\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar
        } else {
            int r_rhs = gen(node->rhs);
            x86_mov_rr(cg_sec, 4, X86_RCX, CG_X86_REG(r_rhs)); // movl %s, %%ecx
            if (node->kind == ND_SHL)
                asm_shl_cl(cg_sec, r_lhs, sz); // %s %%cl, %s\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar
            else if (use_unsigned(node->ty))
                asm_shr_cl(cg_sec, r_lhs, sz);
            else
                asm_sar_cl(cg_sec, r_lhs, sz);
            free_reg(r_rhs);
        }
#endif
        return r_lhs;
    }

    if (node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_MUL ||
        node->kind == ND_BITAND || node->kind == ND_BITXOR || node->kind == ND_BITOR ||
        node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) {
        char *inst = "";
        if (node->kind == ND_ADD) inst = "add";
        else if (node->kind == ND_SUB)
            inst = "sub";
        else if (node->kind == ND_MUL)
            inst =
#ifdef ARCH_ARM64
                "mul";
#else
                "imul";
#endif
        else if (node->kind == ND_BITAND)
            inst = "and";
        else if (node->kind == ND_BITXOR)
            inst =
#ifdef ARCH_ARM64
                "eor";
#else
                "xor";
#endif
        else if (node->kind == ND_BITOR)
            inst =
#ifdef ARCH_ARM64
                "orr";
#else
                "or";
#endif
        else
            inst = "cmp";

        int sz = (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE)
            ? op_size(node->lhs->ty)
            : op_size(node->ty);
        if (sz < op_size(node->rhs->ty))
            sz = op_size(node->rhs->ty);

        if (node->rhs->kind == ND_NUM && node->rhs->val == (int32_t)node->rhs->val) {
            // Skip identity operations: add 0, sub 0, mul 1, and ~0, or 0, xor 0
            int imm = (int)node->rhs->val;
            if ((node->kind == ND_MUL && imm == 1) ||
                ((node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_BITOR || node->kind == ND_BITXOR) && imm == 0)) {
                // no-op, just return r_lhs
#ifdef ARCH_ARM64
            } else if (node->kind == ND_MUL && imm > 0 && (imm & (imm - 1)) == 0) {
                // Strength reduction: multiply by power of 2 → shift
                int shift = 0;
                int tmp = imm;
                while (tmp > 1) {
                    shift++;
                    tmp >>= 1;
                }
                asm_shl_imm(cg_sec, r_lhs, sz, (uint8_t)(reg(r_lhs, sz))); // lsl %s, %s, #%d
            } else if (node->kind == ND_MUL) {
                // ARM64 mul doesn't take immediates; load into a scratch register
                int tmp = alloc_reg();
                emit_mov_imm(reg(tmp, sz), imm);
                asm_mul_reg_reg(cg_sec, r_lhs, r_lhs, sz); // mul %s, %s, %s
                free_reg(tmp);
            } else if (!strcmp(inst, "cmp")) {
                (void)0 /* FIXME: arm64 cmp imm */;
            } else {
                emit_mov_imm64("x16", (uint64_t)(int64_t)imm);
                secbuf_emit32le(cg_sec, arm64_subs_reg(sz == 8 ? 1 : 0, 31, CG_ARM_REG(r_lhs), 16, ARM64_LSL, 0)); // cmp %s, #%d
            }
        } else if (node->kind != ND_BITAND && node->kind != ND_BITOR && node->kind != ND_BITXOR &&
                   imm >= 0 && imm <= 4095) {
            if (!strcmp(inst, "add")) asm_add_imm(cg_sec, r_lhs, sz, imm); // cmn %s, #%d
            else if (!strcmp(inst, "sub"))
                asm_sub_imm(cg_sec, r_lhs, sz, imm); // cmp %s, %s\n", reg(r_lhs, sz), sz <= 4 ? "w16" : "x16
        } else {
            int tmp = alloc_reg();
            emit_mov_imm(reg(tmp, sz), imm);
            if (!strcmp(inst, "add")) asm_add_reg_reg(cg_sec, r_lhs, tmp, sz); // %s %s, %s, #%d
            else if (!strcmp(inst, "sub"))
                asm_sub_reg_reg(cg_sec, r_lhs, tmp, sz); // %s %s, %s, %s
            else if (!strcmp(inst, "and"))
                asm_and_reg_reg(cg_sec, r_lhs, tmp, sz); // lsl %s, %s, #%d
            else if (!strcmp(inst, "orr"))
                asm_or_reg_reg(cg_sec, r_lhs, tmp, sz); // mul %s, %s, %s
            else if (!strcmp(inst, "eor"))
                asm_eor_reg_reg(cg_sec, r_lhs, tmp, sz); // cmp %s, #%d
            free_reg(tmp);
            // } closed by shared } after #endif
#else
            } else if (node->kind == ND_MUL && imm > 0 && (imm & (imm - 1)) == 0) {
                // Strength reduction: multiply by power of 2 → shift
                int shift = 0;
                int tmp = imm;
                while (tmp > 1) {
                    shift++;
                    tmp >>= 1;
                }
                asm_shl_imm(cg_sec, r_lhs, sz, (uint8_t)(shift)); // shl $%d, %s
            } else {
                if (!strcmp(inst, "add")) asm_add_imm(cg_sec, r_lhs, sz, imm); // %s $%d, %s
                else if (!strcmp(inst, "sub"))
                    asm_sub_imm(cg_sec, r_lhs, sz, imm); // sxtw %s, %s
                else if (!strcmp(inst, "imul")) {
                    asm_mov_imm(cg_sec, r_lhs, sz, (int32_t)node->rhs->val); // cmp %s, %s
                    asm_mul_reg_reg(cg_sec, r_lhs, r_lhs, sz); // %s %s, %s, %s
                } else if (!strcmp(inst, "and"))
                    asm_and_imm(cg_sec, r_lhs, sz, imm); // asr %s, %s, #%d
                else if (!strcmp(inst, "or"))
                    asm_or_imm(cg_sec, r_lhs, sz, imm); // sdiv %s, %s, %s
                else if (!strcmp(inst, "xor"))
                    asm_xor_imm(cg_sec, r_lhs, sz, imm); // movslq %s, %s
                else if (!strcmp(inst, "cmp"))
                    asm_cmp_imm(cg_sec, r_lhs, sz, imm); // %s %s, %s
#endif
        }
    } else {
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        // Sign-extend rhs to 64 bits when operation is 64-bit but rhs was
        // computed as 32-bit signed. ARM64: use sxtw.
        if (sz == 8 && op_size(node->rhs->ty) == 4 && !use_unsigned(node->rhs->ty))
            asm_movsx(cg_sec, r_rhs, r_rhs, 8, 4); // shl $%d, %s
        if (!strcmp(inst, "cmp")) {
            asm_cmp_reg_reg(cg_sec, r_lhs, r_rhs, 8); // %s $%d, %s
        } else {
            if (!strcmp(inst, "add")) asm_add_reg_reg(cg_sec, r_lhs, r_rhs, sz); // sxtw %s, %s
            else if (!strcmp(inst, "sub"))
                asm_sub_reg_reg(cg_sec, r_lhs, r_rhs, sz); // cmp %s, %s
            else if (!strcmp(inst, "mul"))
                asm_mul_reg_reg(cg_sec, r_lhs, r_rhs, sz); // %s %s, %s, %s
            else if (!strcmp(inst, "and"))
                asm_and_reg_reg(cg_sec, r_lhs, r_rhs, sz); // asr %s, %s, #%d
            else if (!strcmp(inst, "eor"))
                asm_eor_reg_reg(cg_sec, r_lhs, r_rhs, sz); // sdiv %s, %s, %s
            else if (!strcmp(inst, "orr"))
                asm_or_reg_reg(cg_sec, r_lhs, r_rhs, sz); // movslq %s, %s
        }
        // Pointer subtraction: divide byte difference by element size
        if (node->kind == ND_SUB && node->lhs->ty->base && node->rhs->ty->base) {
            int elem_sz = node->lhs->ty->base->size;
            if (elem_sz > 1) {
                if ((elem_sz & (elem_sz - 1)) == 0) {
                    int shift = 0, tmp = elem_sz;
                    while (tmp > 1) {
                        shift++;
                        tmp >>= 1;
                    }
                    asm_sar_imm(cg_sec, r_lhs, sz, (uint8_t)(reg(r_lhs, sz))); // %s %s, %s
                } else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)elem_sz);
                    asm_div_reg(cg_sec, r_lhs, sz, true);
                    free_reg(tmp);
                }
            }
        }
#else
            // Sign-extend rhs to 64 bits when operation is 64-bit but rhs was
            // computed as 32-bit signed (e.g. pointer + int, long + int).
            // 32-bit writes zero-extend in x86-64, so without this a negative
            // int would be treated as a large positive offset.
            if (sz == 8 && op_size(node->rhs->ty) == 4 && !use_unsigned(node->rhs->ty))
                asm_movsx(cg_sec, r_rhs, r_rhs, 8, 4);
            if (!strcmp(inst, "add")) asm_add_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "sub"))
                asm_sub_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "imul"))
                asm_mul_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "and"))
                asm_and_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "xor"))
                asm_eor_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "or"))
                asm_or_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            else if (!strcmp(inst, "cmp"))
                asm_cmp_reg_reg(cg_sec, r_lhs, r_rhs, sz);
            // Pointer subtraction: divide byte difference by element size
            if (node->kind == ND_SUB && node->lhs->ty->base && node->rhs->ty->base) {
                int elem_sz = node->lhs->ty->base->size;
                if (elem_sz > 1) {
                    if ((elem_sz & (elem_sz - 1)) == 0) {
                        // Power of 2: use arithmetic shift right
                        int shift = 0;
                        int tmp = elem_sz;
                        while (tmp > 1) {
                            shift++;
                            tmp >>= 1;
                        }
                        asm_sar_imm(cg_sec, r_lhs, sz, (uint8_t)(shift)); // sar $%d, %s
                    } else {
                        // Non-power of 2: use idiv
                        x86_mov_rr(cg_sec, sz, X86_RAX, CG_X86_REG(r_lhs)); // mov %s, %s\n", reg(r_lhs, sz), sz == 8 ? "%rax" : "%eax
                        if (sz == 8)
                            x86_cqo(cg_sec); // cqo
                        else
                            x86_cdq(cg_sec); // cdq
                        x86_mov_ri(cg_sec, sz, X86_RCX, elem_sz); // mov $%d, %s\n", elem_sz, sz == 8 ? "%r11" : "%r11d
                        x86_idiv_r(cg_sec, sz, X86_RCX); // idiv %s\n", sz == 8 ? "%r11" : "%r11d
                        x86_mov_rr(cg_sec, sz, CG_X86_REG(r_lhs), X86_RAX); // mov %s, %s\n", sz == 8 ? "%rax" : "%eax
                    }
                }
            }
#endif
        free_reg(r_rhs);
    }

    if (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) {
#ifdef ARCH_ARM64
        const char *cset_cond = "eq";
        if (node->kind == ND_EQ) cset_cond = "eq";
        else if (node->kind == ND_NE)
            cset_cond = "ne";
        else if (node->kind == ND_LT)
            cset_cond = use_unsigned_cmp(node) ? "lo" : "lt";
        else if (node->kind == ND_LE)
            cset_cond = use_unsigned_cmp(node) ? "ls" : "le";
        (void)0 /* FIXME: cset with var cond */;
#else
            X86Cond cond = X86_E;
            if (node->kind == ND_EQ) cond = X86_E;
            else if (node->kind == ND_NE)
                cond = X86_NE;
            else if (node->kind == ND_LT)
                cond = use_unsigned_cmp(node) ? X86_B : X86_L;
            else if (node->kind == ND_LE)
                cond = use_unsigned_cmp(node) ? X86_BE : X86_LE;
            asm_setcc(cg_sec, r_lhs, cond); // cset %s, %s
            asm_movzx(cg_sec, r_lhs, r_lhs, 4, 1); // %s %%al
#endif
    }
    // Extended bitfield arithmetic masking (GCC extension for unsigned long long : N).
    // When both operands involve extended (>= 8-byte underlying type) bitfield reads,
    // mask the result to the max bitfield width, matching GCC's behavior.
    if (node->kind == ND_MUL || node->kind == ND_ADD || node->kind == ND_SUB) {
        int bw = 0;
        Node *ops[2] = {node->lhs, node->rhs};
        for (int oi = 0; oi < 2; oi++) {
            Node *op = ops[oi];
            if (op && op->kind == ND_MEMBER && op->member &&
                op->member->bit_width > 32 && op->member->bit_width < 64 &&
                op->member->ty && op->member->ty->size >= 8 &&
                op->member->ty->is_unsigned) {
                if (op->member->bit_width > bw)
                    bw = op->member->bit_width;
            }
        }
        if (bw > 0) {
            unsigned long long mask = (1ULL << bw) - 1;
#ifdef ARCH_ARM64
            emit_mov_imm64("x16", mask);
            asm_and_reg_reg(cg_sec, r_lhs, 16, 8); // movzbl %%al, %s
#else
                asm_movabs_phy(cg_sec, X86_RAX, (uint64_t)(mask)); // and %s, %s, x16
                asm_and_reg_reg(cg_sec, r_lhs, 0, 8); // movabsq $%llu, %%rax
#endif
        }
    }

    return r_lhs;
}

error("invalid expression %d", node->kind);
return -1;
}

#ifndef ARCH_ARM64
// Helper: map 64-bit register name to 32-bit
static const char *to32(const char *r64) {
    if (r64[0] == '%') r64++;
    if (!strcmp(r64, "rax")) return "%eax";
    if (!strcmp(r64, "rcx")) return "%ecx";
    if (!strcmp(r64, "rdx")) return "%edx";
    if (!strcmp(r64, "r8")) return "%r8d";
    if (!strcmp(r64, "r9")) return "%r9d";
    if (!strcmp(r64, "r10")) return "%r10d";
    if (!strcmp(r64, "r11")) return "%r11d";
    if (!strcmp(r64, "rbx")) return "%ebx";
    if (!strcmp(r64, "r12")) return "%r12d";
    if (!strcmp(r64, "r13")) return "%r13d";
    if (!strcmp(r64, "r14")) return "%r14d";
    if (!strcmp(r64, "r15")) return "%r15d";
    if (!strcmp(r64, "rsi")) return "%esi";
    return NULL;
}
#endif

// Fast peephole parsing helpers (avoid sscanf in hot loop)
static int peep_mov_reg_reg(char *line, char *dst, int dst_sz, char *src, int src_sz) {
    if (strncmp(line, "  mov ", 6) != 0) return 0;
    char *p = line + 6;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int dlen = comma - p;
    if (dlen >= dst_sz) return 0;
    memcpy(dst, p, dlen);
    dst[dlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    int slen = strlen(p);
    if (slen >= src_sz) return 0;
    memcpy(src, p, slen + 1);
    return 1;
}
static int peep_label(char *line, char *lbl, int lbl_sz) {
    char *col = strchr(line, ':');
    if (!col || col == line) return 0;
    int len = col - line;
    if (len >= lbl_sz) return 0;
    memcpy(lbl, line, len);
    lbl[len] = '\0';
    return 1;
}

#ifndef ARCH_ARM64
static int peep_mov_rbp_reg(char *line, int *off, char *reg, int reg_sz) {
    // AT&T: mov[lq] %reg, -N(%rbp)  (store reg to [rbp-N])
    if (strncmp(line, "  mov", 5) != 0) return 0;
    char *p = line + 5;
    if (*p == 'l' || *p == 'q') p++;
    if (*p != ' ') return 0;
    p++;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int rlen = comma - p;
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen);
    reg[rlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    int n;
    if (sscanf(p, "-%d(%%rbp)", &n) == 1) {
        *off = n;
        return 1;
    }
    return 0;
}
static int peep_mov_reg_rbp(char *line, char *dst, int dst_sz, char *szword, int *off) {
    (void)dst_sz;
    // AT&T: mov[lq] -N(%%rbp), %%reg  (load from [rbp-N] into reg)
    if (strncmp(line, "  mov", 5) != 0) return 0;
    char *p = line + 5;
    if (p[0] == 'l') {
        if (szword) strcpy(szword, "dword");
        p++;
    } else if (p[0] == 'q') {
        if (szword) strcpy(szword, "qword");
        p++;
    } else
        return 0;
    if (*p != ' ') return 0;
    p++;
    if (sscanf(p, "-%d(%%rbp), %s", off, dst) == 2)
        return 1;
    return 0;
}
static int peep_mov_rbp_imm(char *line, int *off, int *val) {
    // AT&T: movl $imm, -N(%%rbp)
    if (sscanf(line, "  movl $%d, -%d(%%rbp)", val, off) == 2) return 1;
    return 0;
}
static int peep_jmp(char *line, char *lbl, int lbl_sz) {
    if (strncmp(line, "  jmp ", 6) != 0) return 0;
    char *p = line + 6;
    int len = strlen(p);
    if (len >= lbl_sz) return 0;
    memcpy(lbl, p, len + 1);
    return 1;
}
static int peep_mov_reg_imm(char *line, char *reg, int reg_sz, int *imm) {
    // AT&T: mov[lq]? $imm, %reg
    if (strncmp(line, "  mov", 5) != 0) return 0;
    char *p = line + 5;
    if (*p == 'l' || *p == 'q') p++;
    if (*p != ' ') return 0;
    p++;
    if (*p != '$') return 0;
    p++;
    char *endp;
    long v = strtol(p, &endp, 0);
    if (endp == p) return 0;
    *imm = (int)v;
    if (*endp != ',') return 0;
    p = endp + 1;
    while (*p == ' ' || *p == '\t') p++;
    int rlen = strlen(p);
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen + 1);
    return 1;
}
#endif

static int peep_op_reg_reg(char *line, char *op, int op_sz, char *dst, int dst_sz, char *src, int src_sz) {
    if (line[0] != ' ' || line[1] != ' ') return 0;
    char *p = line + 2;
    char *sp = strchr(p, ' ');
    if (!sp) return 0;
    int olen = sp - p;
    if (olen >= op_sz) return 0;
    memcpy(op, p, olen);
    op[olen] = '\0';
    p = sp + 1;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int dlen = comma - p;
    if (dlen >= dst_sz) return 0;
    memcpy(dst, p, dlen);
    dst[dlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    int slen = strlen(p);
    if (slen >= src_sz) return 0;
    memcpy(src, p, slen + 1);
    return 1;
}
#ifndef ARCH_ARM64
static int peep_mov_reg_mem(char *line, char *reg, int reg_sz, char *mem, int mem_sz) {
    // AT&T: mov[lq]? mem, %reg  (load from mem into reg)
    if (strncmp(line, "  mov", 5) != 0) return 0;
    char *p = line + 5;
    if (*p == 'l' || *p == 'q') p++;
    if (*p != ' ') return 0;
    p++;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int mlen = comma - p;
    if (mlen >= mem_sz) return 0;
    memcpy(mem, p, mlen);
    mem[mlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    int rlen = strlen(p);
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen + 1);
    return 1;
}
#endif

// Helper: check if string is a register name
static int is_reg(const char *s) {
    if (s[0] == '%') s++;
#ifndef ARCH_ARM64
    // x86 registers
    if (s[0] == 'r' || s[0] == 'e' || !strncmp(s, "si", 2) || !strncmp(s, "bl", 2) || !strncmp(s, "bx", 2))
        return 1;
#else
    // ARM64 registers: xN, wN
    if ((s[0] == 'x' || s[0] == 'w') && s[1] >= '0' && s[1] <= '9')
        return 1;
    if ((s[0] == 'x' || s[0] == 'w') && s[1] == '1' && s[2] >= '0' && s[2] <= '9')
        return 1;
#endif
    return 0;
}

// Check if a physical register is used after line 'after' before being overwritten

// ARM64 peep helpers (parser-style, no sscanf)
#ifdef ARCH_ARM64
// Match "  str %[^,], [x29, #-N]" → fill reg, offset, return 1
static int peep_arm_str_fp(const char *line, char *reg, int reg_sz, int *off) {
    if (strncmp(line, "  str ", 6) != 0) return 0;
    const char *p = line + 6;
    const char *comma = strchr(p, ',');
    if (!comma) return 0;
    int rlen = comma - p;
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen);
    reg[rlen] = '\0';
    p = comma + 1;
    while (*p == ' ') p++;
    return sscanf(p, "[x29, #-%d]", off) == 1;
}
// Match "  ldr %[^,], [x29, #-N]" → fill reg, offset, return 1
static int peep_arm_ldr_fp(const char *line, char *reg, int reg_sz, int *off) {
    if (strncmp(line, "  ldr ", 6) != 0) return 0;
    const char *p = line + 6;
    const char *comma = strchr(p, ',');
    if (!comma) return 0;
    int rlen = comma - p;
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen);
    reg[rlen] = '\0';
    p = comma + 1;
    while (*p == ' ') p++;
    return sscanf(p, "[x29, #-%d]", off) == 1;
}
// Match "  b .LABEL" → fill label, return 1
static int peep_arm_b(const char *line, char *lbl, int lbl_sz) {
    if (strncmp(line, "  b .", 5) != 0) return 0;
    int len = strlen(line + 5);
    if (len >= lbl_sz) return 0;
    memcpy(lbl, line + 5, len + 1);
    return 1;
}
#endif

// === Peephole Patterns 2-5 ===

// Pattern 2: store REG, [fp-N]; load REG2, [fp-N] → mov REG2, REG
static int peep_pattern2(char **lines, int li, int lj) {
#ifdef ARCH_ARM64
    char sr[32], dr[32];
    int off1, off2;
    if (peep_arm_str_fp(lines[li], sr, sizeof(sr), &off1) &&
        peep_arm_ldr_fp(lines[lj], dr, sizeof(dr), &off2) &&
        off1 == off2) {
        // Reject mismatched sizes (e.g. str xN,[fp] / ldr wN,[fp]) because
        // mov wN, xN is not a valid ARM64 instruction.
        if (sr[0] != dr[0])
            return 0;
        lines[lj] = format("  mov %s, %s", dr, sr);
        return 1;
    }
#else
    int off1, off2;
    char sr[32], dr[32], sz[16];
    if (peep_mov_rbp_reg(lines[li], &off1, sr, sizeof(sr))) {
        if (peep_mov_reg_rbp(lines[lj], dr, sizeof(dr), sz, &off2) &&
            off1 == off2 && !strcmp(sz, "dword")) {
            const char *r32 = to32(sr);
            if (r32) {
                lines[lj] = format("  mov %s, %s", r32, dr);
                return 1;
            }
        }
        if (peep_mov_reg_rbp(lines[lj], dr, sizeof(dr), sz, &off2) &&
            off1 == off2 && !strcmp(sz, "qword")) {
            lines[lj] = format("  mov %s, %s", sr, dr);
            return 1;
        }
    }
    { // 2b: movl $VAL, -N(%rbp); movl -N(%rbp), %REG
        int off1, off2, val;
        char dr[32], sz[16];
        if (peep_mov_rbp_imm(lines[li], &off1, &val) &&
            peep_mov_reg_rbp(lines[lj], dr, sizeof(dr), sz, &off2) &&
            off1 == off2 && !strcmp(sz, "dword")) {
            lines[lj] = format("  movl $%d, %s", val, dr);
            return 1;
        }
    }
#endif
    return 0;
}

// Pattern 3: jmp/b .LABEL; .LABEL: → delete branch
static int peep_pattern3(char **lines, int li, int lj) {
    char lbl1[80], lbl2[80];
#ifdef ARCH_ARM64
    if (!peep_arm_b(lines[li], lbl1, sizeof(lbl1))) return 0;
#else
    if (!peep_jmp(lines[li], lbl1, sizeof(lbl1))) return 0;
#endif
    if (!peep_label(lines[lj], lbl2, sizeof(lbl2))) return 0;
    char *t = lbl2;
    while (*t == ' ') t++;
    if (!strcmp(lbl1, t)) {
        lines[li][0] = '\0';
        return 1;
    }
    return 0;
}

// Pattern 4: mov REG, #IMM; OP REG2, REG → OP REG2, #IMM
static int peep_pattern4(char **lines, int li, int lj) {
    char rd[32];
    int imm_val;
#ifdef ARCH_ARM64
    (void)li;
    (void)lj;
    if (sscanf(lines[li], " mov %31s, #%d", rd, &imm_val) != 2) return 0;
    // We'd need peep_op to match arm64 3-op; skip for now
    return 0;
#else
    if (!peep_mov_reg_imm(lines[li], rd, sizeof(rd), &imm_val) || !is_reg(rd))
        return 0;
    long v_check = strtol(strchr(lines[li], '$') + 1, NULL, 0);
    if (v_check != (int)v_check) return 0;
    char op[16], od[64], os[32];
    if (!peep_op_reg_reg(lines[lj], op, sizeof(op), od, sizeof(od), os, sizeof(os)) ||
        !same_phys(od, rd)) return 0;
    char *opname = op;
    int olen = strlen(op);
    if (olen > 1 && (op[olen - 1] == 'l' || op[olen - 1] == 'q' || op[olen - 1] == 'w' || op[olen - 1] == 'b')) {
        static char opbase[16];
        memcpy(opbase, op, olen - 1);
        opbase[olen - 1] = '\0';
        opname = opbase;
    }
    if (!strcmp(opname, "cmp") || !strcmp(opname, "add") || !strcmp(opname, "sub") ||
        !strcmp(opname, "and") || !strcmp(opname, "or") || !strcmp(opname, "xor") ||
        !strcmp(opname, "imul")) {
        if (((!strcmp(opname, "add") || !strcmp(opname, "sub") || !strcmp(opname, "or") || !strcmp(opname, "xor")) && imm_val == 0) ||
            (!strcmp(opname, "imul") && imm_val == 1)) {
            lines[lj][0] = '\0';
        } else {
            lines[lj] = format("  %s $%d, %s", op, imm_val, os);
        }
        // Conservative: skip dead-predecessor deletion without forward scan
        return 1;
    }
#endif
    return 0;
}

// Scan up to 30 lines of remaining body text (newlines still intact) for pid use.
static int reg_live_in_text(const char *text, int pid) {
    const char *variants[6] = {NULL};
    int nv = 0;
    for (int vi = 0; vi < NUM_REGS; vi++) {
        if (phys_reg_id(reg64[vi]) == pid) {
            variants[nv++] = reg64[vi];
            variants[nv++] = reg32[vi];
            break;
        }
    }
#ifndef ARCH_ARM64
    if (pid == 0) {
        variants[nv++] = "%rax";
        variants[nv++] = "%eax";
        variants[nv++] = "%al";
    } else if (pid == 1) {
        variants[nv++] = "%rcx";
        variants[nv++] = "%ecx";
        variants[nv++] = "%cl";
    } else if (pid == 2) {
        variants[nv++] = "%rdx";
        variants[nv++] = "%edx";
    } else if (pid == 3) {
        variants[nv++] = "%rbx";
        variants[nv++] = "%ebx";
        variants[nv++] = "%bl";
    }
#endif
    const char *p = text;
    for (int cnt = 0; cnt < 30 && *p; cnt++) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            if (*p != ' ') return 1; // label = block boundary, conservatively live
            for (int vi = 0; vi < nv && variants[vi]; vi++) {
                size_t vlen = strlen(variants[vi]);
                for (size_t i = 0; i + vlen <= len; i++) {
                    if (memcmp(p + i, variants[vi], vlen) == 0) return 1;
                }
            }
        }
        p = nl ? nl + 1 : p + len;
        if (!nl) break;
    }
    return 0;
}

// Pattern 5: mov R, [mem]; OP R, IMMx; mov dst, R → mov dst, [mem]; OP dst, IMMx
// rest: unprocessed body text after the window (newlines still intact) for liveness scan
static int peep_pattern5(char **lines, int li, int lj, int lk, const char *rest) {
    char r1[32], mem1[128], op2[16], r2[32], imm2[32], d3[32], r3[32];
#ifndef ARCH_ARM64
    if (peep_mov_reg_mem(lines[li], r1, sizeof(r1), mem1, sizeof(mem1)) &&
        peep_op_reg_reg(lines[lj], op2, sizeof(op2), r2, sizeof(r2), imm2, sizeof(imm2)) &&
        peep_mov_reg_reg(lines[lk], d3, sizeof(d3), r3, sizeof(r3)) &&
        !strcmp(r1, imm2) && !strcmp(imm2, r3) &&
        is_reg(d3) && !is_reg(mem1)) {
        int r1_pid = phys_reg_id(r1);
        if (r1_pid >= 0 && reg_live_in_text(rest, r1_pid))
            return 0;
        lines[li] = format("  mov %s, %s", mem1, d3);
        lines[lj] = format("  %s %s, %s", op2, r2, d3);
        lines[lk][0] = '\0';
        return 1;
    }
#else
    // ARM64: ldr R, [mem]; OP R, R, #IMM; mov dst, R -> ldr dst, [mem]; OP dst, dst, #IMM
    if (sscanf(lines[li], " ldr %31[^,], %127[^\n]", r1, mem1) == 2 &&
        peep_op_reg_reg(lines[lj], op2, sizeof(op2), r2, sizeof(r2), imm2, sizeof(imm2)) &&
        peep_mov_reg_reg(lines[lk], d3, sizeof(d3), r3, sizeof(r3)) &&
        !strcmp(r1, r2) && !strcmp(r2, r3) &&
        is_reg(d3) && !is_reg(mem1)) {
        // Verify the middle instruction is OP R, R, ... (second operand must be R)
        char *p = imm2;
        while (*p == ' ' || *p == '\t') p++;
        int r2len = strlen(r2);
        if (strncmp(p, r2, r2len) != 0 || (p[r2len] != '\0' && p[r2len] != ',' && p[r2len] != ' '))
            return 0;
        // Must be a 3-operand instruction (imm2 contains a second comma after r2)
        if (strchr(p + r2len, ',') == NULL)
            return 0;
        int r1_pid = phys_reg_id(r1);
        if (r1_pid >= 0 && reg_live_in_text(rest, r1_pid))
            return 0;
        char *val = strchr(imm2, ',');
        if (val) {
            val++;
            while (*val == ' ') val++;
        } else
            val = imm2;
        lines[li] = format("  ldr %s, %s", d3, mem1);
        lines[lj] = format("  %s %s, %s, %s", op2, d3, d3, val);
        lines[lk][0] = '\0';
        return 1;
    }
#endif
    return 0;
}

static void emit_peephole_body(char *body_text) {
    if (opt_O0) {
        fputs(body_text, stdout);
        return;
    }

    // 3-line sliding window: w[0]=oldest pending, w[2]=newest
    char *w[3] = {NULL, NULL, NULL};
    int wn = 0;
    const char *rest = body_text; // points into body_text after window (newlines intact)
    char *p = body_text; // scan pointer; \n replaced with \0 as lines are parsed

    for (;;) {
        char *line = NULL;
        if (*p) {
            char *nl = strchr(p, '\n');
            if (nl) {
                *nl = '\0';
                line = p;
                p = nl + 1;
            } else {
                line = p;
                p += strlen(p);
            }
            rest = p;
        }

        if (!line) {
            for (int i = 0; i < wn; i++)
                if (w[i] && w[i][0]) fprintf(stdout, "%s\n", w[i]);
            break;
        }

        // Commit oldest when window is full
        if (wn == 3) {
            if (w[0] && w[0][0]) fprintf(stdout, "%s\n", w[0]);
            w[0] = w[1];
            w[1] = w[2];
            w[2] = NULL;
            wn = 2;
        }
        w[wn++] = line;

        // Repeatedly try patterns on all adjacent non-empty pairs in the window
        bool any;
        do {
            any = false;
            for (int ii = 0; ii < wn; ii++) {
                if (!w[ii] || !w[ii][0]) continue;
                int jj = ii + 1;
                while (jj < wn && (!w[jj] || !w[jj][0])) jj++;
                if (jj >= wn) break;
                int kk = jj + 1;
                while (kk < wn && (!w[kk] || !w[kk][0])) kk++;

                // Pattern 1: mov SRC, REG; mov REG, DST -> mov SRC, DST
                {
                    char d1[80], s1[80], d2[80], s2[80];
                    if (peep_mov_reg_reg(w[ii], d1, sizeof(d1), s1, sizeof(s1)) &&
                        peep_mov_reg_reg(w[jj], d2, sizeof(d2), s2, sizeof(s2)) &&
                        !strcmp(d2, s1) && strcmp(d1, d2) && is_reg(d1) && is_reg(s1) && is_reg(s2)) {
                        w[jj] = format("  mov %s, %s", d1, s2);
                        // Conservative: skip dead-predecessor deletion without full forward scan
                        any = true;
                        break;
                    }
                }
                // Pattern 2: store REG, [fp-N]; load REG2, [fp-N] -> mov REG2, REG
                if (peep_pattern2(w, ii, jj)) {
                    any = true;
                    break;
                }
                // Pattern 3: jmp/b .LABEL; .LABEL: -> delete branch
                if (peep_pattern3(w, ii, jj)) {
                    any = true;
                    break;
                }
                // Pattern 4: mov REG, IMM; OP REG2, REG -> OP REG2, IMM
                if (peep_pattern4(w, ii, jj)) {
                    any = true;
                    break;
                }
                // Pattern 5: mov R, [mem]; OP R, IMMx; mov dst, R -> mov dst, [mem]; OP dst, IMMx
                if (kk < wn && peep_pattern5(w, ii, jj, kk, rest)) {
                    any = true;
                    break;
                }
            }
        } while (any);
    }
}

static const char *node_kind_name(NodeKind k) {
    switch (k) {
    case ND_ADD: return "ADD";
    case ND_SUB: return "SUB";
    case ND_MUL: return "MUL";
    case ND_DIV: return "DIV";
    case ND_MOD: return "MOD";
    case ND_SHL: return "SHL";
    case ND_SHR: return "SHR";
    case ND_BITAND: return "BITAND";
    case ND_BITXOR: return "BITXOR";
    case ND_BITOR: return "BITOR";
    case ND_EQ: return "EQ";
    case ND_NE: return "NE";
    case ND_LT: return "LT";
    case ND_LE: return "LE";
    case ND_ASSIGN: return "ASSIGN";
    case ND_POST_INC: return "POST_INC";
    case ND_POST_DEC: return "POST_DEC";
    case ND_ADDR: return "ADDR";
    case ND_DEREF: return "DEREF";
    case ND_CAST: return "CAST";
    case ND_BITNOT: return "BITNOT";
    case ND_FUNCALL: return "FUNCALL";
    case ND_LVAR: return "LVAR";
    case ND_NUM: return "NUM";
    case ND_RETURN: return "RETURN";
    case ND_IF: return "IF";
    case ND_FOR: return "FOR";
    case ND_DO: return "DO";
    case ND_SWITCH: return "SWITCH";
    case ND_CASE: return "CASE";
    case ND_BREAK: return "BREAK";
    case ND_CONTINUE: return "CONTINUE";
    case ND_GOTO: return "GOTO";
    case ND_GOTO_IND: return "GOTO_IND";
    case ND_LABEL: return "LABEL";
    case ND_LABEL_VAL: return "LABEL_VAL";
    case ND_STMT_EXPR: return "STMT_EXPR";
    case ND_BLOCK: return "BLOCK";
    case ND_EXPR_STMT: return "EXPR_STMT";
    case ND_NULL: return "NULL";
    case ND_STR: return "STR";
    case ND_MEMBER: return "MEMBER";
    case ND_LOGAND: return "LOGAND";
    case ND_LOGOR: return "LOGOR";
    case ND_COND: return "COND";
    case ND_COMMA: return "COMMA";
    case ND_SIZEOF: return "SIZEOF";
    case ND_FNUM: return "FNUM";
    case ND_NEG: return "NEG";
    case ND_NOT: return "NOT";
    case ND_ZERO_INIT: return "ZERO_INIT";
    case ND_ASM: return "ASM";
    case ND_VA_START: return "VA_START";
    case ND_VA_COPY: return "VA_COPY";
    case ND_VA_ARG: return "VA_ARG";
    case ND_ALLOCA: return "ALLOCA";
    case ND_ALLOCA_ZINIT: return "ALLOCA_ZINIT";
    case ND_CHAIN: return "CHAIN";
    case ND_ATOMIC_LOAD: return "ATOMIC_LOAD";
    case ND_ATOMIC_STORE: return "ATOMIC_STORE";
    case ND_ATOMIC_EXCHANGE: return "ATOMIC_EXCHANGE";
    case ND_ATOMIC_CAS: return "ATOMIC_CAS";
    case ND_ATOMIC_FENCE: return "ATOMIC_FENCE";
    case ND_ATOMIC_FETCH_OP: return "ATOMIC_FETCH_OP";
    default: return "UNKNOWN";
    }
}

static void dump_node(FILE *f, Node *node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) fputc(' ', f);
    fprintf(f, "%s", node_kind_name(node->kind));
    if (node->kind == ND_NUM) fprintf(f, " %lld", (long long)node->val);
    if (node->kind == ND_LVAR && node->var) fprintf(f, " '%s'", node->var->name ? node->var->name : "(anon)");
    if (node->kind == ND_FUNCALL && node->lhs && node->lhs->kind == ND_LVAR && node->lhs->var)
        fprintf(f, " %s", node->lhs->var->name);
    fprintf(f, "\n");
    if (node->kind == ND_BLOCK)
        for (Node *n = node->body; n; n = n->next) dump_node(f, n, depth + 2);
    if (node->lhs) dump_node(f, node->lhs, depth + 2);
    if (node->rhs) dump_node(f, node->rhs, depth + 2);
    if (node->then) dump_node(f, node->then, depth + 2);
    if (node->els) dump_node(f, node->els, depth + 2);
    if (node->cond) dump_node(f, node->cond, depth + 2);
    if (node->body) dump_node(f, node->body, depth + 2);
    if (node->init) dump_node(f, node->init, depth + 2);
    if (node->inc) dump_node(f, node->inc, depth + 2);
    if (node->kind == ND_CASE && node->lhs) dump_node(f, node->lhs, depth + 2);
}

void dump_ast(Program *prog) {
    fprintf(stderr, "=== AST dump ===\n");
    for (TLItem *item = prog->items; item; item = item->next) {
        if (item->kind == TL_FUNC && item->fn->body) {
            fprintf(stderr, "function %s:\n", item->fn->name);
            dump_node(stderr, item->fn->body, 2);
        }
    }
    fprintf(stderr, "=== end AST dump ===\n");
}

struct ObjFile *codegen(Program *prog) {
    // Reset label and fixup hashtables for new compilation unit
    cg_label_ht_reset();
    asm_fixup_ht_reset();

    // Initialize binary ObjFile for asm_* emission
    static ObjFile _obj;
    cg_obj = &_obj;
    memset(cg_obj, 0, sizeof(*cg_obj));
    objfile_init(cg_obj);
    cg_set_section(SEC_TEXT);
    cg_dry_run = false;

    all_items = prog->items;
    all_strs = prog->strs;
    alloca_needed = false;
    debug_file_count = 0;
    last_debug_file = 0;
    last_debug_line = 0;
    // Assembly header
#ifndef ARCH_ARM64
    /* AT&T syntax is now the default */
#endif
#ifndef _WIN32
#if !defined(__APPLE__)
    /* .note.GNU-stack: default in ELF, no bytes emitted */
#endif
#endif

    // Emit data section for strings
    if (prog->globals || prog->strs || float_lits) {
        (void)0 /* section directive */;
        // Track emitted symbols to avoid duplicates (e.g. __asm__("same_name"))
        char **emitted_syms = NULL;
        int emitted_count = 0;
        for (LVar *var = prog->globals; var; var = var->next) {
            if (var->is_extern && !var->alias_target && !var->asm_name)
                continue;
            char *label = var->asm_name ? var->asm_name : var->name;
            if (var->is_function && !var->alias_target && !var->asm_name)
                continue;
            // Handle function aliases (__attribute__((alias)) or __asm__ renaming)
            if (var->is_function && !var->alias_target && var->asm_name) {
                // __asm__("target") on a function: alias the C name to the asm_name
                (void)0 /* TODO: .set symbol alias via objfile_add_sym */;
                continue;
            }
            if (var->alias_target) {
                (void)0 /* TODO: .set symbol alias via objfile_add_sym */;
                continue;
            }
            // If a global with this asm_name already emitted, skip (alias target)
            bool sym_already_emitted = false;
            const char *canon = asm_sym_name(sym_name(label));
            for (int i = 0; i < emitted_count; i++) {
                if (strcmp(emitted_syms[i], canon) == 0) {
                    sym_already_emitted = true;
                    break;
                }
            }
            if (sym_already_emitted)
                continue;
            if (var->asm_name) {
                // Check if asm_name matches another global that provides data
                LVar *existing = NULL;
                for (LVar *g = prog->globals; g; g = g->next) {
                    if (g != var && !g->is_extern && !g->is_function &&
                        strcmp(g->name, var->asm_name) == 0 &&
                        (g->has_init || g->init_data)) {
                        existing = g;
                        break;
                    }
                }
                if (existing) {
                    // This is an alias via __asm__ — emit .set instead of data
                    (void)0 /* .globl symbol handled by objfile */;
                    (void)0 /* .set directive */;
                    continue;
                }
            }
            char **new_syms = realloc(emitted_syms, (emitted_count + 1) * sizeof(char *));
            if (!new_syms) {
                fprintf(stderr, "realloc failed\n");
                exit(1);
            }
            emitted_syms = new_syms;
            emitted_syms[emitted_count++] = (char *)canon;
            bool reserved = !var->asm_name && is_asm_reserved(var->name);
            char *safe_label = reserved ? format(".L_rcc_%s", var->name) : label;
            int is_bss = (!var->init_data && !var->relocs && !var->has_init && var->ty->size > 0);
            const char *sym_name_str = asm_sym_name(sym_name(safe_label)); // .balign %d
            if (is_bss) {
                size_t align = var->ty->align > 1 ? var->ty->align : 1;
                size_t rem = cg_obj->bss_size % align;
                if (rem) cg_obj->bss_size += align - rem;
                size_t off = cg_obj->bss_size;
                if (!var->is_static)
                    objfile_add_sym(cg_obj, sym_name_str, SEC_BSS, off, var->ty->size, SB_GLOBAL, ST_OBJECT);
                else
                    objfile_add_sym(cg_obj, sym_name_str, SEC_BSS, off, var->ty->size, SB_LOCAL, ST_OBJECT);
                cg_label_ht_add(sym_name_str, off);
                if (reserved)
                    objfile_add_sym(cg_obj, asm_sym_name(sym_name(label)), SEC_BSS, off, var->ty->size, var->is_static ? SB_LOCAL : SB_GLOBAL, ST_OBJECT); // .globl %s
                cg_obj->bss_size += var->ty->size;
            } else {
                cg_set_section(SEC_DATA);
                secbuf_align(cg_sec, var->ty->align > 1 ? var->ty->align : 1);
                size_t off = cg_sec->len;
                if (!var->is_static)
                    objfile_add_sym(cg_obj, sym_name_str, SEC_DATA, off, var->ty->size, SB_GLOBAL, ST_OBJECT);
                else
                    objfile_add_sym(cg_obj, sym_name_str, SEC_DATA, off, var->ty->size, SB_LOCAL, ST_OBJECT);
                cg_label_ht_add(sym_name_str, off);
                if (reserved)
                    objfile_add_sym(cg_obj, asm_sym_name(sym_name(label)), SEC_DATA, off, var->ty->size, var->is_static ? SB_LOCAL : SB_GLOBAL, ST_OBJECT); // %s:
                if (var->init_data || var->relocs) {
                    int pos = 0;
                    for (Reloc *rel = var->relocs; rel; rel = rel->next) {
                        for (; pos < rel->offset; pos++)
                            secbuf_emit8(cg_sec, (uint8_t)var->init_data[pos]); // .set %s, %s
                        size_t rel_off = cg_sec->len;
                        secbuf_emit64le(cg_sec, (uint64_t)rel->addend); // .byte %u
                        int sidx = objfile_find_sym(cg_obj, rel->label);
                        if (sidx < 0)
                            sidx = objfile_add_sym(cg_obj, rel->label, SEC_UNDEF, 0, 0, SB_GLOBAL, ST_NOTYPE);
                        objfile_add_reloc(cg_obj, SEC_DATA, rel_off, sidx, R_X86_64_64, 0);
                        pos += 8;
                    }
                    for (; pos < var->init_size; pos++)
                        secbuf_emit8(cg_sec, (uint8_t)var->init_data[pos]); // .quad %s%+d
                    if (var->ty->size > var->init_size) {
                        size_t pad = var->ty->size - var->init_size;
                        secbuf_reserve(cg_sec, pad);
                        memset(cg_sec->data + cg_sec->len, 0, pad);
                        cg_sec->len += pad;
                    }
                } else if (var->has_init) {
                    if (var->ty->size == 1)
                        secbuf_emit8(cg_sec, (uint8_t)var->init_val); // .quad %s
                    else if (var->ty->size == 2)
                        secbuf_emit16le(cg_sec, (uint16_t)var->init_val); // .quad %s%+d
                    else if (var->ty->size == 4)
                        secbuf_emit32le(cg_sec, (uint32_t)var->init_val); // .quad %s
                    else
                        secbuf_emit64le(cg_sec, (uint64_t)var->init_val); // .byte %u
                }
            }
        }
        free(emitted_syms);
        cg_set_section(SEC_RODATA);
        for (StrLit *s = prog->strs; s; s = s->next) {
            cg_def_label_sec(format(".LC%d", s->id), SEC_RODATA); // .zero %d
            if (s->prefix != 0) {
                // Wide string: decode UTF-8 and emit wide characters
                char *p = s->str;
                while (*p) {
                    char *next;
                    uint32_t c = decode_utf8(&next, p);
                    if (next == p) {
                        p++;
                        continue;
                    } // invalid UTF-8, skip
                    p = next;
                    if (s->elem_size == 2) {
                        // UTF-16: Windows wchar_t is UCS-2 (2 bytes).
                        // Code points above U+FFFF need a surrogate pair.
                        if (c > 0xFFFF) {
                            uint32_t sc = c - 0x10000;
                            secbuf_emit16le(cg_sec, (uint16_t)(0xD800 + (sc >> 10))); // .2byte %u
                            secbuf_emit16le(cg_sec, (uint16_t)(0xDC00 + (sc & 0x3FF))); // .2byte %u
                        } else {
                            secbuf_emit16le(cg_sec, (uint16_t)c); // .2byte %u
                        }
                    } else {
                        secbuf_emit32le(cg_sec, c); // .4byte %u
                    }
                }
                if (s->elem_size == 2)
                    secbuf_emit16le(cg_sec, 0); // .2byte 0
                else
                    secbuf_emit32le(cg_sec, 0); // .4byte 0
            } else {
                secbuf_emitbuf(cg_sec, s->str, s->len); // .byte %u
                secbuf_emit8(cg_sec, 0); // .byte 0
            }
        }
        cg_set_section(SEC_TEXT);
    }

    for (TLItem *item = prog->items; item; item = item->next) {
        if (item->kind == TL_ASM) {
            // Translate TCC-specific {$} to immediate in Intel syntax
            const char *tp = item->asm_str;
            while (*tp) {
                if (tp[0] == '{' && tp[1] == '$' && tp[2] == '}') {
                    tp += 3;
                } else {
                    putchar(*tp++);
                }
            }
            putchar('\n');
            continue;
        }
        Function *fn = item->fn;
        current_fn = fn->name;
        current_fn_def = fn;
        current_fn_stack_size = fn->stack_size;
#ifndef ARCH_ARM64
        init_spill_slots();
#endif
        fn_struct_ret_off = 0;
        fn_struct_ret_total = 0;

        // Pass 1: Generate function body to temp file to discover register usage
        // Pass 1: Discover register usage (dry run to dummy buffer)
        SecBuf _dummy;
        secbuf_init(&_dummy);
        SecBuf *saved_sec = cg_sec;
        cg_sec = &_dummy;
        cg_dry_run = true;
        used_regs = 0;
        ever_used_regs = 0;
        spilled_regs = 0;
        spill_count = 0;
        memset(reg_owner, 0, sizeof(reg_owner));
        ctrl_depth = 0;
        fn_uses_alloca = false;
        last_debug_file = 0;
        last_debug_line = 0;

        // Save params to locals (emitted to body buffer, will be after prologue)
        // ARM64: handled in the Pass 2 prologue instead
#ifndef ARCH_ARM64
        int param_xmm_index = 0;
        int stack_param_index = 0;
        int param_index = fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION) ? 1 : 0;
#ifdef _WIN32
        char *param_regs64[] = {"%rcx", "%rdx", "%r8", "%r9"};
        char *param_regs32[] = {"%ecx", "%edx", "%r8d", "%r9d"};
        char *param_regs16[] = {"%cx", "%dx", "%r8w", "%r9w"};
        char *param_regs8[] = {"%cl", "%dl", "%r8b", "%r9b"};
        char *param_xmm[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3"};
        int max_param_regs = 4;
#else
        char *param_regs64[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        char *param_regs32[] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
        char *param_regs16[] = {"%di", "%si", "%dx", "%cx", "%r8w", "%r9w"};
        char *param_regs8[] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
        char *param_xmm[] = {"%xmm0", "%xmm1", "%xmm2", "%xmm3", "%xmm4", "%xmm5", "%xmm6", "%xmm7"};
        X86Reg cg_x86_paramreg[] = {X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9};
        int max_param_regs = 6;
        int max_param_xmm = 8;
#endif
        for (LVar *var = fn->params; var; var = var->param_next) {
#ifdef _WIN32
            if (is_flonum(var->ty)) {
                if (param_index < max_param_regs) {
                    if (var->ty->size == 4) {
                        x86_cvtsd2ss(cg_sec, X86_XMM0, X86_XMM0); // cvtsd2ss %s, %%xmm0
                        (void)0 /* FIXME: float op */;
                    } else {
                        (void)0 /* FIXME: float op */;
                    }
                    param_index++;
                    param_xmm_index++;
                }
            } else if (param_index < max_param_regs) {
                char *preg = var->ty->size == 1 ? param_regs8[param_index]
                    : var->ty->size == 2        ? param_regs16[param_index]
                    : var->ty->size <= 4        ? param_regs32[param_index]
                                                : param_regs64[param_index];
                // Structs > 8 bytes are passed by pointer; copy to local stack
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                    int c = ++rcc_label_count;
                    (void)0 /* FIXME: movq preg to r11 */;
                    x86_mov_ri(cg_sec, 8, X86_R10, var->ty->size); // movq %s, %%r11
                    cg_def_label(format(".L.param.%d", c)); // movq $%d, %%r10
                    asm_cmp_zero(cg_sec, 10, 8); // .L.pcopy.%d:
                    asm_jcc_label(cg_sec, X86_E); // cmpq $0, %%r10
                    (void)0 /* FIXME: sized mov */;
                    (void)0 /* FIXME: sized mov */;
                    asm_dec(cg_sec, 10, 8); // je .L.pcopy_end.%d
                    asm_jmp_label(cg_sec); // movb -1(%%r11,%%r10), %%al
                    cg_def_label(format(".L.param_end.%d", c)); // movb %%al, -%d-1(%%rbp,%%r10)
                } else {
                    (void)0 /* FIXME: mov indirect/mem */;
                }
                param_index++;
            } else {
                // Stack argument on Windows (shadow space = 32 bytes)
                int stack_off = 48 + stack_param_index * 8;
                if (is_flonum(var->ty)) {
                    if (var->ty->size == 4) {
                        (void)0 /* FIXME: float op */;
                        (void)0 /* FIXME: float op */;
                    } else {
                        (void)0 /* FIXME: float op */;
                        (void)0 /* FIXME: float op */;
                    }
                } else {
                    char *tmpreg = var->ty->size == 1 ? "%al"
                        : var->ty->size == 2          ? "%ax"
                        : var->ty->size <= 4          ? "%eax"
                                                      : "%rax";
                    (void)0 /* FIXME: mov indirect/mem */;
                    (void)0 /* FIXME: mov indirect/mem */;
                }
                stack_param_index++;
            }
#else
            if (is_flonum(var->ty)) {
                if (param_xmm_index < max_param_xmm) {
                    if (var->ty->size == 4) {
                        x86_cvtsd2ss(cg_sec, X86_XMM0, X86_XMM0); // cvtsd2ss %s, %%xmm0
                        (void)0 /* FIXME: float op */;
                    } else {
                        (void)0 /* FIXME: float op */;
                    }
                    param_xmm_index++;
                } else {
                    if (var->ty->size == 4) {
                        (void)0 /* FIXME: float op */;
                        (void)0 /* FIXME: float op */;
                    } else {
                        (void)0 /* FIXME: float op */;
                        (void)0 /* FIXME: float op */;
                    }
                    stack_param_index++;
                }
            } else if (param_index < max_param_regs) {
                char *preg = var->ty->size == 1 ? param_regs8[param_index]
                    : var->ty->size == 2        ? param_regs16[param_index]
                    : var->ty->size <= 4        ? param_regs32[param_index]
                                                : param_regs64[param_index];
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                    int c = ++rcc_label_count;
                    (void)0 /* FIXME: movq preg to r11 */;
                    x86_mov_ri(cg_sec, 8, X86_R10, var->ty->size); // movq %s, %%r11
                    cg_def_label(format(".L.param.%d", c)); // movq $%d, %%r10
                    asm_cmp_zero(cg_sec, 10, 8); // .L.pcopy.%d:
                    asm_jcc_label(cg_sec, X86_E); // cmpq $0, %%r10
                    (void)0 /* FIXME: sized mov */;
                    (void)0 /* FIXME: sized mov */;
                    asm_dec(cg_sec, 10, 8); // je .L.pcopy_end.%d
                    asm_jmp_label(cg_sec); // movb -1(%%r11,%%r10), %%al
                    cg_def_label(format(".L.param_end.%d", c)); // movb %%al, -%d-1(%%rbp,%%r10)
                } else {
                    (void)0 /* FIXME: mov indirect/mem */;
                }
                param_index++;
            } else {
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                    int c = ++rcc_label_count;
                    x86_mov_ri(cg_sec, 8, X86_R10, var->ty->size); // subq $1, %%r10
                    cg_def_label(format(".L.param.%d", c)); // jmp .L.pcopy.%d
                    asm_cmp_zero(cg_sec, 10, 8); // .L.pcopy_end.%d:
                    asm_jcc_label(cg_sec, X86_E); // mov %s, -%d(%%rbp)
                    (void)0 /* FIXME: sized mov */;
                    (void)0 /* FIXME: sized mov */;
                    asm_dec(cg_sec, 10, 8); // movq $%d, %%r10
                    asm_jmp_label(cg_sec); // .L.pcopy.%d:
                    cg_def_label(format(".L.param_end.%d", c)); // cmpq $0, %%r10
                } else {
                    char *tmpreg = var->ty->size == 1 ? "%al"
                        : var->ty->size == 2          ? "%ax"
                        : var->ty->size <= 4          ? "%eax"
                                                      : "%rax";
                    (void)0 /* FIXME: mov indirect/mem */;
                    (void)0 /* FIXME: mov indirect/mem */;
                }
                stack_param_index++;
            }
#endif
        }
#endif /* !ARCH_ARM64 */

#ifndef ARCH_ARM64
        // Save arg registers if function is variadic
        if (fn->is_variadic) {
            int gp_count = param_index;
            int va_fp = param_xmm_index;
            va_reg_save_ofs = current_fn_stack_size + 176;
            va_gp_start = gp_count * 8;
            va_fp_start = va_fp * 16 + 48;
            va_st_start = 16 + stack_param_index * 8;

            switch (gp_count) {
            case 0: asm_mov_phyreg_rbp(cg_sec, X86_RDI, 8, va_reg_save_ofs); /* fallthrough */ /* movq %%rcx, -%d(%%rbp)\n */
            case 1: asm_mov_phyreg_rbp(cg_sec, X86_RSI, 8, va_reg_save_ofs - 8); /* fallthrough */ /* movq %%rdx, -%d(%%rbp)\n */
            case 2: asm_mov_phyreg_rbp(cg_sec, X86_RDX, 8, va_reg_save_ofs - 16); /* fallthrough */ /* movq %%r8, -%d(%%rbp)\n */
            case 3: asm_mov_phyreg_rbp(cg_sec, X86_RCX, 8, va_reg_save_ofs - 24); /* fallthrough */ /* movq %%r9, -%d(%%rbp)\n */
            case 4: asm_mov_phyreg_rbp(cg_sec, X86_R8, 8, va_reg_save_ofs - 32); /* fallthrough */ /* movaps %%xmm0, -%d(%%rbp)\n */
            case 5: asm_mov_phyreg_rbp(cg_sec, X86_R9, 8, va_reg_save_ofs - 40); // movaps %%xmm1, -%d(%%rbp)
            }
            if (va_fp < 8) {
                x86_test_rr(cg_sec, 1, X86_RAX, X86_RAX); // movaps %%xmm2, -%d(%%rbp)
                asm_jcc_label(cg_sec, X86_E); // movaps %%xmm3, -%d(%%rbp)
                switch (va_fp) {
                case 0: asm_movaps_rbp_xmm(cg_sec, 0, va_reg_save_ofs - 48); /* fallthrough */ /* movq %%rdi, -%d(%%rbp)\n */
                case 1: asm_movaps_rbp_xmm(cg_sec, 1, va_reg_save_ofs - 64); /* fallthrough */ /* movq %%rsi, -%d(%%rbp)\n */
                case 2: asm_movaps_rbp_xmm(cg_sec, 2, va_reg_save_ofs - 80); /* fallthrough */ /* movq %%rdx, -%d(%%rbp)\n */
                case 3: asm_movaps_rbp_xmm(cg_sec, 3, va_reg_save_ofs - 96); /* fallthrough */ /* movq %%rcx, -%d(%%rbp)\n */
                case 4: asm_movaps_rbp_xmm(cg_sec, 4, va_reg_save_ofs - 112); /* fallthrough */ /* movq %%r8, -%d(%%rbp)\n */
                case 5: asm_movaps_rbp_xmm(cg_sec, 5, va_reg_save_ofs - 128); /* fallthrough */ /* movq %%r9, -%d(%%rbp)\n */
                case 6: asm_movaps_rbp_xmm(cg_sec, 6, va_reg_save_ofs - 144); /* fallthrough */ /* testb %%al, %%al\n */
                case 7: asm_movaps_rbp_xmm(cg_sec, 7, va_reg_save_ofs - 160); // je .L.va_no_xmm.%d
                }
                cg_def_label(format(".L.x%d", rcc_label_count++)); // movaps %%xmm0, -%d(%%rbp)
            }
        }
#else
        // Compute va_list init values for ARM64 (register saves are in Pass 2)
        if (fn->is_variadic) {
            int gp_param = 0;
            int fp_param = 0;
            int stack_param = 0;
            for (LVar *var = fn->params; var; var = var->param_next) {
                if (is_flonum(var->ty)) {
                    if (fp_param < 8) fp_param++;
                } else if (gp_param < 8) {
                    gp_param++;
                } else {
                    stack_param++;
                }
            }
#ifdef __APPLE__
            // Apple ARM64: unnamed variadic args go on the stack, not in registers.
            // Set va offsets past the register save area so va_arg immediately
            // reads from the overflow (stack) area.
            va_gp_start = 64;
            va_fp_start = 192;
#else
            // AArch64 ABI: gr_offs = -(8 - gp_param) * 8, vr_offs = -(8 - fp_param) * 16
            va_gp_start = -(8 - gp_param) * 8;
            va_fp_start = -(8 - fp_param) * 16;
#endif
            va_st_start = 16 + stack_param * 8;
        }
#endif

        for (Node *n = fn->body; n; n = n->next) {
            int r = gen(n);
            if (r != -1) free_reg(r);
        }
        // Pass 2: Emit binary prologue, body, epilogue
        cg_sec = saved_sec;
        secbuf_free(&_dummy);
        cg_dry_run = false;

#ifdef ARCH_ARM64
        // === ARM64 prologue ===
        // Callee-saved: x19-x28 are callee-saved among our 12 allocatable regs
        // x19-x24 = indices 6-11 in our reg arrays
        int callee_mask = ((ever_used_regs >> 6) & 63);
        int n_callee_saved = 0;
        for (int j = 0; j < 6; j++)
            if (callee_mask & (1 << j)) n_callee_saved++;

        int need = fn->stack_size + fn_struct_ret_total + 32;
        int va_save_size = fn->is_variadic ? 192 : 0;
        if (fn->is_variadic)
            need += va_save_size;
        int frame_size = need + 16 + n_callee_saved * 8;
        // Round up to 16-byte alignment
        frame_size = (frame_size + 15) & ~15;
        // va_reg_save_ofs not used on ARM64; reg_save_area is stored as sp in va_start

        // Symbol linkage
        bool has_noninline_decl = false;
        bool had_extern_decl = false;
        if (fn->is_inline && !fn->is_extern) {
            for (LVar *g = prog->globals; g; g = g->next) {
                if (g->is_function && strcmp(g->name, fn->name) == 0) {
                    if (g->has_init) has_noninline_decl = true;
                    if (g->is_extern && !g->is_weak) had_extern_decl = true;
                    break;
                }
            }
        }
        char *fn_label = fn->name;
        if (is_asm_reserved(fn->name))
            fn_label = format(".L_rcc_%s", fn->name);
        bool fn_exported = !fn->is_static && (!fn->is_inline || fn->is_extern || has_noninline_decl || had_extern_decl);
        if (fn->is_weak) {
            cg_weak_label(asm_sym_name(sym_name(fn->name))); // .weak_definition %s
        } else if (fn_exported)
            cg_global_label(asm_sym_name(sym_name(fn->name))); // .globl %s
        else
            cg_def_label(asm_sym_name(sym_name(fn_label))); // .weak %s

        // Stack frame: stp fp,lr; mov fp,sp; sub sp,sp,#frame_size
        (void)0 /* FIXME: ldp/stp */;
        asm_mov_reg_reg(cg_sec, 29, 31, 8 if 1 == "1" else 4); // .globl %s
        if (frame_size <= 4095)
            asm_sub_imm(cg_sec, 31, 8 if 1 == "1" else 4, frame_size); // %s = %s
        else {
            int fs = frame_size;
            asm_mov_imm(cg_sec, 16, 8, fs & 0xffff); // %s = %s
            fs >>= 16;
            int s = 16;
            while (fs) {
                asm_movk(cg_sec, 16, 1, (uint16_t)(fs & 0xffff), s); // .p2align 2
                fs >>= 16;
                s += 16;
            }
            (void)0 /* FIXME: sub with phy reg */;
        }

        // Save variadic argument registers at the bottom of the frame (sp)
        if (fn->is_variadic) {
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            (void)0 /* FIXME: ldp/stp */;
            // Save original sp so va_start can find the reg_save_area even after alloca
            (void)0 /* FIXME: mov phy */;
            (void)0 /* FIXME: sized ld/st */;
        }

        // Save callee-saved regs
        int cs_off = fn->is_variadic ? 192 + 16 : 16;
        for (int j = 0; j < 6; j++) {
            if (callee_mask & (1 << j)) {
                (void)0 /* FIXME: ldr/str phy/off */;
                cs_off += 8;
            }
        }

        // Save hidden struct return pointer if needed
        if (fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION)) {
            int retbuf_offset = 0;
            for (LVar *var = fn->locals; var; var = var->next) {
                if (var->name && strcmp(var->name, "__retbuf") == 0) {
                    retbuf_offset = var->offset;
                    break;
                }
            }
            if (retbuf_offset <= 4095)
                (void)0 /* FIXME: ldr/str phy/off */;
            else {
                int v = retbuf_offset;
                asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // str x8, [%s, #-%d]
                v >>= 16;
                int s = 16;
                while (v) {
                    asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // mov x16, #%d
                    v >>= 16;
                    s += 16;
                }
                (void)0 /* FIXME: sub with phy reg */;
                (void)0 /* FIXME: ldr/str phy/off */;
            }
        }

        // Save incoming params to stack slots
        int gp_param = 0;
        int fp_param = 0;
        int stack_param = 0;
        {
            char *gpreg[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
            char *wpreg[] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
            char *fpreg[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
            for (LVar *var = fn->params; var; var = var->param_next) {
                int hfa_elem_size = 0;
                int hfa_count = (!is_flonum(var->ty) && (var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION))
                    ? arm64_hfa_count(var->ty, &hfa_elem_size)
                    : 0;
                if (hfa_count > 0 && fp_param + hfa_count <= 8) {
                    if (var->offset <= 4095)
                        (void)0 /* FIXME: sub with phy reg */;
                    else {
                        int v = var->offset;
                        asm_mov_imm(cg_sec, 16, 8, v & 0xffff); // sub x16, %s, #%d
                        v >>= 16;
                        int s = 16;
                        while (v) {
                            asm_movk(cg_sec, 16, 1, (uint16_t)(v & 0xffff), s); // mov x16, #%d
                            v >>= 16;
                            s += 16;
                        }
                        (void)0 /* FIXME: sub with phy reg */;
                    }
                    for (int j = 0; j < hfa_count; j++) {
                        int off = j * hfa_elem_size;
                        if (hfa_elem_size == 4)
                            (void)0 /* FIXME: ldr/str phy/off */;
                        else
                            (void)0 /* FIXME: ldr/str phy/off */;
                    }
                    fp_param += hfa_count;
                } else if (is_flonum(var->ty)) {
                    if (fp_param < 8) {
                        if (var->ty->size == 4) {
                            if (fn->ty->is_oldstyle) {
                                (void)0 /* FIXME: float op */;
                                (void)0 /* FIXME: ldr/str phy/off */;
                            } else {
                                (void)0 /* FIXME: ldr/str phy/off */;
                            }
                        } else
                            (void)0 /* FIXME: ldr/str phy/off */;
                        fp_param++;
                    }
                } else if (gp_param < 8) {
                    char *preg = var->ty->size <= 4 ? wpreg[gp_param] : gpreg[gp_param];
                    if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                        int c = ++rcc_label_count;
                        (void)0 /* FIXME: mov phy */;
                        if (var->offset <= 4095)
                            (void)0 /* FIXME: sub with phy reg */;
                        else {
                            int v = var->offset;
                            asm_mov_imm(cg_sec, 13, 8, v & 0xffff); // movk x16, #%d, lsl #%d
                            v >>= 16;
                            int s = 16;
                            while (v) {
                                asm_movk(cg_sec, 13, 1, (uint16_t)(v & 0xffff), s); // sub x16, %s, x16
                                v >>= 16;
                                s += 16;
                            }
                            (void)0 /* FIXME: sub with phy reg */;
                        }
                        asm_mov_imm(cg_sec, 9, 8, var->ty->size); // str s%d, [x16, #%d]
                        (void)0 /* FIXME: label .L.xxx.c */;
                        asm_cmp_zero(cg_sec, 9, 8); // str d%d, [x16, #%d]
                        asm_jcc_label(cg_sec, ARM64_EQ); // fcvt s0, d%d
                        asm_dec(cg_sec, 9, 8); // str s0, [%s, #-%d]
                        (void)0 /* FIXME: sized ld/st */;
                        (void)0 /* FIXME: sized ld/st */;
                        asm_jmp_label(cg_sec); // str s%d, [%s, #-%d]
                        (void)0 /* FIXME: label .L.xxx.c */;
                    } else {
                        (void)0 /* FIXME: ldr/str phy/off */;
                    }
                    gp_param++;
                } else {
                    // Stack argument — load from caller's frame using correct width
                    int spoff = 16 + stack_param * 8;
                    if (is_flonum(var->ty)) {
                        if (var->ty->size == 4) {
                            (void)0 /* FIXME: ldr/str phy/off */;
                            (void)0 /* FIXME: ldr/str phy/off */;
                        } else {
                            (void)0 /* FIXME: ldr/str phy/off */;
                            (void)0 /* FIXME: ldr/str phy/off */;
                        }
                    } else if (var->ty->size <= 1) {
                        (void)0 /* FIXME: sized ld/st */;
                        (void)0 /* FIXME: sized ld/st */;
                    } else if (var->ty->size <= 2) {
                        (void)0 /* FIXME: sized ld/st */;
                        (void)0 /* FIXME: sized ld/st */;
                    } else if (var->ty->size <= 4) {
                        (void)0 /* FIXME: ldr/str phy/off */;
                        (void)0 /* FIXME: ldr/str phy/off */;
                    } else {
                        (void)0 /* FIXME: ldr/str phy/off */;
                        (void)0 /* FIXME: ldr/str phy/off */;
                    }
                    stack_param++;
                }
            }
        }


        // Re-run gen() to emit binary body
        {
            uint64_t _t0 = opt_time ? cg_now_us() : 0;
            for (Node *n = fn->body; n; n = n->next) {
                asm_peep_node_start(cg_sec); // .L.return.%s:
                int r = gen(n);
                asm_peep_node_end(cg_sec); // .L.return.%s:
                if (r != -1) free_reg(r);
            }
            if (opt_time)
                time_peep_us += cg_now_us() - _t0;
        }
        // Re-run gen() to emit binary body
        {
            uint64_t _t0 = opt_time ? cg_now_us() : 0;
            for (Node *n = fn->body; n; n = n->next) {
                asm_peep_node_start(cg_sec);
                int r = gen(n);
                asm_peep_node_end(cg_sec);
                if (r != -1) free_reg(r);
            }
            if (opt_time)
                time_peep_us += cg_now_us() - _t0;
        }

        // === ARM64 epilogue ===
        cg_def_label(format(".L.return.%s", fn->name));

        // Cleanup calls
        bool has_cleanup = false;
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var)) {
                has_cleanup = true;
                break;
            }
        if (has_cleanup)
            (void)0 /* FIXME: ldr/str phy/off */;
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var))
                emit_cleanup_var(var);
        if (has_cleanup)
            (void)0 /* FIXME: ldr/str phy/off */;

        // VLA or alloca may have moved sp; restore to fixed frame position
        // before reading callee-saved regs (stored at sp+16..sp+n at frame entry)
        if (fn->dealloc_vla || fn_uses_alloca) {
            if (frame_size <= 4095)
                asm_sub_imm(cg_sec, 31, 8 if 1 == "1" else 4, frame_size); // str x0, [%s, #-8]
            else {
                int fs = frame_size;
                asm_mov_imm(cg_sec, 16, 8, fs & 0xffff); // ldr x0, [%s, #-8]
                fs >>= 16;
                int s = 16;
                while (fs) {
                    asm_movk(cg_sec, 16, 1, (uint16_t)(fs & 0xffff), s); // sub %s, %s, #%d
                    fs >>= 16;
                    s += 16;
                }
                asm_sub_reg_reg(cg_sec, 31, 16, 8 if 1 == "1" else 4); // mov x16, #%d
            }
        }

        // Restore callee-saved
        cs_off = 16;
        for (int j = 0; j < 6; j++) {
            if (callee_mask & (1 << j)) {
                (void)0 /* FIXME: ldr/str phy/off */;
                cs_off += 8;
            }
        }

        if (frame_size <= 4095)
            asm_add_imm(cg_sec, 31, 8 if 1 == "1" else 4, frame_size); // ldr %s, [%s, #%d]
        else {
            int fs = frame_size;
            asm_mov_imm(cg_sec, 16, 8, fs & 0xffff); // add %s, %s, #%d
            fs >>= 16;
            int s = 16;
            while (fs) {
                asm_movk(cg_sec, 16, 1, (uint16_t)(fs & 0xffff), s); // mov x16, #%d
                fs >>= 16;
                s += 16;
            }
            asm_add_reg_reg(cg_sec, 31, 16, 8 if 1 == "1" else 4); // movk x16, #%d, lsl #%d
        }
        (void)0 /* FIXME: ldp/stp */;
        asm_ret(cg_sec); // add %s, %s, x16

#else
        // === x86_64 prologue ===
        // Determine which callee-saved regs need saving.
        // Windows: rbx,r12,r13,r14,r15,rsi (indices 2-7)
        // Linux SysV: rbx,r12,r13,r14,r15 only (rsi is caller-saved)
#ifdef _WIN32
        int callee_count = 6;
#else
        int callee_count = 5;
#endif
        int callee_mask = (ever_used_regs >> 2) & ((1 << callee_count) - 1);
        int n_pushes = 0;
        for (int j = 0; j < callee_count; j++)
            if (callee_mask & (1 << j)) n_pushes++;

        // Calculate stack frame size
        // Total space below rbp must cover: locals + spills + shadow (32)
        int need = fn->stack_size + fn_struct_ret_total + 32;
        if (fn->is_variadic)
            need = va_reg_save_ofs;
        // Reserve space for register spill slots
        if (need < next_spill_slot)
            need = next_spill_slot;
        int push_bytes = n_pushes * 8;
        int sub_amount = need - push_bytes;
        if (sub_amount < 32) sub_amount = 32;
        // Fix 16-byte alignment
        if ((push_bytes + sub_amount) % 16 != 0) sub_amount += 8;

        // Emit prologue - handle is_weak, inline, and static linkage.
        // For inline functions, check:
        // 1. If there's a non-inline declaration (has_init) in globals
        // 2. If prior non-weak declaration had extern (LVar is_extern=true)
        bool has_noninline_decl = false;
        bool had_extern_decl = false;
        if (fn->is_inline && !fn->is_extern) {
            for (LVar *g = prog->globals; g; g = g->next) {
                if (g->is_function && strcmp(g->name, fn->name) == 0) {
                    if (g->has_init)
                        has_noninline_decl = true;
                    // Only consider non-weak extern declarations
                    if (g->is_extern && !g->is_weak)
                        had_extern_decl = true;
                    break;
                }
            }
        }
        char *fn_label = fn->name;
        if (is_asm_reserved(fn->name))
            fn_label = format(".L_rcc_%s", fn->name);
        bool fn_exported = !fn->is_static && (!fn->is_inline || fn->is_extern || has_noninline_decl || had_extern_decl);
        if (fn->is_weak) {
            cg_weak_label(asm_sym_name(sym_name(fn->name))); // .weak_definition %s
        } else if (fn_exported) {
            cg_global_label(asm_sym_name(sym_name(fn->name))); // .globl %s
        } else {
            cg_def_label(asm_sym_name(sym_name(fn_label))); // .weak %s
        }
        asm_push_phy(cg_sec, X86_RBP); // .globl %s
        x86_mov_rr(cg_sec, 8, X86_RBP, X86_RSP); /* movq %rsp, %rbp */ /* %s = %s\n */

        // Only push callee-saved registers that were actually used
        for (int j = 0; j < callee_count; j++) {
            if (callee_mask & (1 << j))
                asm_push_phy(cg_sec, CG_X86_REG(j + 2)); // %s = %s
        }
        x86_sub_ri(cg_sec, 8, X86_RSP, sub_amount); // .p2align 2

        // Save incoming params from ABI regs to stack slots
        {
            X86Reg greg[] = {X86_RDI, X86_RSI, X86_RDX, X86_RCX, X86_R8, X86_R9};
            int gp = fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION) ? 1 : 0;
            for (LVar *var = fn->params; var; var = var->param_next) {
                if (!is_flonum(var->ty) && gp < 6 && !((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8)) {
                    int sz = var->ty->size <= 4 ? 4 : 8;
                    x86_mov_mr(cg_sec, sz, x86_mem(X86_RBP, -var->offset), greg[gp]); // %s:
                    gp++;
                }
            }
        }

        if (fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION)) {
            int retbuf_offset = 0;
            for (LVar *var = fn->locals; var; var = var->next) {
                if (var->name && strcmp(var->name, "__retbuf") == 0) {
                    retbuf_offset = var->offset;
                    break;
                }
            }
            char *retreg =
#ifdef _WIN32
                "%rcx"
#else
                "%rdi"
#endif
                ;
            (void)0 /* FIXME: mov indirect/mem */;
        }

        // Re-run gen() to emit binary body
        {
            uint64_t _t0 = opt_time ? cg_now_us() : 0;
            for (Node *n = fn->body; n; n = n->next) {
                asm_peep_node_start(cg_sec); // mov %s, -%d(%%rbp)
                int r = gen(n);
                asm_peep_node_end(cg_sec); // .L.return.%s:
                if (r != -1) free_reg(r);
            }
            if (opt_time)
                time_peep_us += cg_now_us() - _t0;
        }

        // Emit epilogue
        cg_def_label(format(".L.return.%s", fn->name)); // mov %s, -%d(%%rbp)

        // Emit __cleanup__ calls (LIFO: locals list is in reverse declaration order)
        bool has_cleanup = false;
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var)) {
                has_cleanup = true;
                break;
            }
        if (has_cleanup)
            asm_mov_phyreg_rbp(cg_sec, X86_RAX, 8, spill_offset(0)); // movq %%rax, -%d(%%rbp)
        for (LVar *var = fn->locals; var; var = var->next) {
            if (var_has_cleanup(var))
                emit_cleanup_var(var);
        }
        if (has_cleanup)
            asm_mov_rbp_phyreg(cg_sec, X86_RAX, 8, spill_offset(0)); // movq -%d(%%rbp), %%rax
        if (fn->dealloc_vla)
            asm_lea_rbp_phy(cg_sec, X86_RSP, 8, n_pushes * 8); // leaq -%d(%%rbp), %%rsp
        else if (fn_uses_alloca)
            asm_lea_rbp_phy(cg_sec, X86_RSP, 8, n_pushes * 8); // leaq -%d(%%rbp), %%rsp
        else
            x86_add_ri(cg_sec, 8, X86_RSP, sub_amount); // addq $%d, %%rsp
        for (int j = callee_count - 1; j >= 0; j--) {
            if (callee_mask & (1 << j))
                asm_pop_phy(cg_sec, CG_X86_REG(j + 2)); // pop %s
        }
        asm_pop_phy(cg_sec, X86_RBP); // popq %%rbp
        asm_ret(cg_sec); // ret
#endif
    }

    // Emit constructor/destructor entries
    bool has_ctor = false, has_dtor = false;
    for (TLItem *item = prog->items; item; item = item->next) {
        if (item->kind == TL_FUNC) {
            if (item->fn->is_constructor) has_ctor = true;
            if (item->fn->is_destructor) has_dtor = true;
        }
    }
    if (has_ctor) {
        cg_set_section(SEC_DATA);
        for (TLItem *item = prog->items; item; item = item->next) {
            if (item->kind == TL_FUNC && item->fn->is_constructor)
                (void)0 /* directive: .quad */;
        }
    }
    if (has_dtor) {
        cg_set_section(SEC_DATA);
        for (TLItem *item = prog->items; item; item = item->next) {
            if (item->kind == TL_FUNC && item->fn->is_destructor)
                (void)0 /* directive: .quad */;
        }
    }

    if (alloca_needed)
        emit_alloca();

    // Emit float literal constants after all functions
    if (float_lits) {
        cg_set_section(SEC_RODATA);
        (void)0 /* directive: .balign */;
        for (FloatLit *fl = float_lits; fl; fl = fl->next) {
            cg_def_label_sec(format(".LF%d", fl->id), SEC_RODATA); // \n.section .rdata,\"dr\"
            if (fl->size == 4) {
                float f = (float)fl->val;
                unsigned int bits;
                memcpy(&bits, &f, 4);
                (void)0 /* directive: .long */;
                (void)0 /* directive: .long */;
            } else {
                unsigned long long bits;
                memcpy(&bits, &fl->val, 8);
                (void)0 /* directive: .quad */;
            }
        }
    }
    return cg_obj;
}
