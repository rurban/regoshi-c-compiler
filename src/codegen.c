// SPDX-License-Identifier: LGPL-2.1-or-later
// Derived from chibicc by Rui Ueyama.
#include "rcc.h"
#include <stdarg.h>

static FILE *cg_stream;
static Function *current_fn_def;
static TLItem *all_items;
static StrLit *all_strs;
static void cg_emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg_stream, fmt, ap);
    va_end(ap);
}
#define printf(...) cg_emit(__VA_ARGS__)

static char *current_fn;
static int current_fn_stack_size = 0; // fn->stack_size of the function being generated
static int fn_struct_ret_off = 0; // next free offset within the struct-ret-buf area
static int fn_struct_ret_total = 0; // high-water mark of struct-ret-buf space used
static int rcc_label_count = 0;
static int va_gp_start;
static int va_fp_start;
static int va_st_start;
static int va_reg_save_ofs;
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
static char *reg64[] = {"r10", "r11", "rbx", "r12", "r13", "r14", "r15", "rsi"};
static char *reg32[] = {"r10d", "r11d", "ebx", "r12d", "r13d", "r14d", "r15d", "esi"};
static char *reg16[] = {"r10w", "r11w", "bx", "r12w", "r13w", "r14w", "r15w", "si"};
static char *reg8[] = {"r10b", "r11b", "bl", "r12b", "r13b", "r14b", "r15b", "sil"};
#define NUM_REGS 8
#define FRAME_PTR "rbp"
#define STACK_REG "rsp"
#endif

static int used_regs = 0;
static int ever_used_regs = 0;

#ifdef ARCH_ARM64
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
// Spill slot offsets from rbp for register spilling
#define SPILL_R10 56
#define SPILL_R11 64
#define SPILL_RBX 72
#define SPILL_R12 80
#define SPILL_R13 88
#define SPILL_R14 96
#define SPILL_R15 104
#define SPILL_RSI 112

static int spill_offset(int r) {
    static int offsets[] = {SPILL_R10, SPILL_R11, SPILL_RBX,
                            SPILL_R12, SPILL_R13, SPILL_R14,
                            SPILL_R15, SPILL_RSI};
    return offsets[r];
}
#endif

static int spilled_regs = 0;
static int spill_count = 0;

static char *reg(int r, int size);
static int alloc_reg(void);
static void free_reg(int i);
static int gen(Node *node);
static int gen_addr(Node *node);
static bool is_asm_reserved(const char *name);

static char *func_asm_name(char *name) {
    for (TLItem *item = all_items; item; item = item->next) {
        if (item->kind == TL_FUNC && item->fn->name == name)
            return item->fn->asm_name ? item->fn->asm_name : item->fn->name;
    }
    return name;
}

// Mach-O symbols get a leading underscore
static const char *sym_name(const char *name) {
#if defined(__APPLE__)
    if (name[0] == '.' || name[0] == '/')
        return name;
    if (name[0] == '_') return name;
    return format("_%s", name);
#else
    return name;
#endif
}

static void emit_direct_call(char *name) {
    if (is_asm_reserved(name))
        name = format(".L_rcc_%s", name);
#ifdef ARCH_ARM64
    printf("  bl %s\n", sym_name(func_asm_name(name)));
#else
    printf("  call %s\n", sym_name(func_asm_name(name)));
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
            printf("  add x0, %s, #%d\n", FRAME_PTR, -var->offset);
        else {
            int v = var->offset;
            printf("  mov x16, #%d\n", v & 0xffff);
            v >>= 16;
            int s = 16;
            while (v) {
                printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                v >>= 16;
                s += 16;
            }
            printf("  sub x0, %s, x16\n", FRAME_PTR);
        }
#elif defined(_WIN32)
        printf("  lea rcx, [rbp-%d]\n", var->offset);
#else
        printf("  lea rdi, [rbp-%d]\n", var->offset);
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
            printf("  add x0, %s, #%d\n", FRAME_PTR, -off);
        else {
            int v = off;
            printf("  mov x16, #%d\n", v & 0xffff);
            v >>= 16;
            int s = 16;
            while (v) {
                printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                v >>= 16;
                s += 16;
            }
            printf("  sub x0, %s, x16\n", FRAME_PTR);
        }
#elif defined(_WIN32)
        printf("  lea rcx, [rbp-%d]\n", var->offset - i * elem_size);
#else
        printf("  lea rdi, [rbp-%d]\n", var->offset - i * elem_size);
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

// Restore RSP past any VLAs leaving scope (enables VLA reuse on re-entry).
// Locals list is newest-first; the last VLA in range is the outermost one.
static void emit_vla_dealloc(LVar *begin, LVar *end) {
    LVar *outermost_vla = NULL;
    for (LVar *v = begin; v && v != end; v = v->next)
        if (v->ty->kind == TY_VLA)
            outermost_vla = v;
    if (outermost_vla)
        printf("  mov rsp, [rbp-%d]\n", outermost_vla->offset);
}

/* Other platforms still have it. windows deprecated it.
   Use a unique name to avoid conflicts with CRT import stubs. */
static void emit_alloca(void) {
#ifdef ARCH_ARM64
    printf("\n"
           "__rcc_alloca:\n"
           "  // alloca(size) — x0 = rounded size, returns new sp in x0\n"
           "  add x0, x0, #15\n"
           "  and x0, x0, #-16\n"
           "  sub sp, sp, x0\n"
           "  mov x0, sp\n"
           "  ret\n");
#else
    printf("\n"
           "__rcc_alloca:\n"
           "  pop rdx\n"
#ifdef _WIN32
           "  mov rax, rcx\n"
#else
           "  mov rax, rdi\n"
#endif
           "  add rax, 15\n"
           "  and rax, -16\n"
           "  jz .Lalloca3\n"
#ifdef _WIN32
           ".Lalloca1:\n"
           "  cmp rax, 4096\n"
           "  jb .Lalloca2\n"
           "  test [rsp-4096], rax\n"
           "  sub rsp, 4096\n"
           "  sub rax, 4096\n"
           "  jmp .Lalloca1\n"
#endif
           ".Lalloca2:\n"
           "  sub rsp, rax\n"
           "  mov rax, rsp\n"
           ".Lalloca3:\n"
           "  push rdx\n"
           "  ret\n");
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
    Node *argv[64];
    int nargs = 0;
    int arg_regs[64];
    int arg_sizes[64];
    bool arg_is_float[64];
#ifndef _WIN32
    int arg_gp_idx[64];
    int arg_fp_idx[64];
    int arg_stack_idx[64];
#endif
    for (Node *arg = node->args; arg; arg = arg->next)
        argv[nargs++] = arg;

    char *call_target = node->funcname;
    if (call_target && is_asm_reserved(call_target))
        call_target = format(".L_rcc_%s", call_target);
    if (!call_target && node->lhs && node->lhs->kind == ND_LVAR &&
        node->lhs->var && node->lhs->var->is_function)
        call_target = var_label(node->lhs->var);

#ifdef _WIN32
    char *argreg32[] = {"ecx", "edx", "r8d", "r9d"};
    char *argreg64[] = {"rcx", "rdx", "r8", "r9"};
    char *argxmm[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
    int max_gp_args = 4;
    int max_fp_args = 4;
    int shadow_space = 32;
#elif defined(ARCH_ARM64)
    // AAPCS64: 8 GP arg regs (x0-x7), 8 SIMD/FP arg regs (v0-v7)
    // x8 is the indirect result register for struct returns
    char *argreg32[] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
    char *argreg64[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
    char *argxmm[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
    int max_gp_args = 8;
    int max_fp_args = 8;
    int shadow_space = 0;
#else
    char *argreg32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
    char *argreg64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    char *argxmm[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
    int max_gp_args = 6;
    int max_fp_args = 8;
    int shadow_space = 0;
#endif

    bool has_hidden_retbuf = node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION);

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
                printf("  mov %s, %s\n", reg64[r], reg64[dst_r]);

                printf("  cld\n");
                printf("  push rdi\n");
                if (is_memcpy) printf("  push rsi\n");
                printf("  push rcx\n");
                printf("  mov rdi, %s\n", reg64[dst_r]);
                printf("  mov rcx, %s\n", reg64[len_r]);
                if (is_memset) {
                    printf("  movzx eax, %s\n", reg8[v2_r]);
                    printf("  rep stosb\n");
                } else {
                    printf("  mov rsi, %s\n", reg64[v2_r]);
                    printf("  rep movsb\n");
                }
                printf("  pop rcx\n");
                if (is_memcpy) printf("  pop rsi\n");
                printf("  pop rdi\n");

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

                printf("  cld\n");
                printf("  push rdi\n");
                printf("  push rsi\n");
                printf("  push rcx\n");
                printf("  mov rdi, %s\n", reg64[s1_r]);
                printf("  mov rsi, %s\n", reg64[s2_r]);
                printf("  mov rcx, %s\n", reg64[len_r]);
                printf("  repe cmpsb\n");
                printf("  jne .L.memcmp_diff.%d\n", ++rcc_label_count);
                printf("  xor eax, eax\n");
                printf("  jmp .L.memcmp_end.%d\n", rcc_label_count);
                printf(".L.memcmp_diff.%d:\n", rcc_label_count);
                printf("  movsx eax, byte ptr [rdi-1]\n");
                printf("  movsx ecx, byte ptr [rsi-1]\n");
                printf("  sub eax, ecx\n");
                printf(".L.memcmp_end.%d:\n", rcc_label_count);
                printf("  pop rcx\n");
                printf("  pop rsi\n");
                printf("  pop rdi\n");

                free_reg(s1_r);
                free_reg(s2_r);
                free_reg(len_r);

                int r = alloc_reg();
                printf("  mov %s, eax\n", reg32[r]);
                return r;
            }
        }

        if (is_strlen) {
            Node *str = node->args;
            if (str && !str->next) {
                int str_r = gen(str);

                printf("  cld\n");
                printf("  push rdi\n");
                printf("  push rcx\n");
                printf("  mov rdi, %s\n", reg64[str_r]);
                printf("  xor al, al\n");
                printf("  mov rcx, -1\n");
                printf("  repne scasb\n");
                printf("  not rcx\n");
                printf("  dec rcx\n");
                printf("  mov rax, rcx\n");
                printf("  pop rcx\n");
                printf("  pop rdi\n");

                free_reg(str_r);

                int r = alloc_reg();
                printf("  mov %s, rax\n", reg64[r]);
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
                printf("  push rdi\n");
                printf("  push rsi\n");
                printf("  mov rdi, %s\n", reg64[r]);
                printf("  mov rsi, %s\n", reg64[r2]);
                printf(".L.strcmp_loop.%d:\n", cl);
                printf("  mov al, byte ptr [rdi]\n");
                printf("  cmp al, byte ptr [rsi]\n");
                printf("  jne .L.strcmp_diff.%d\n", cl);
                printf("  test al, al\n");
                printf("  jz .L.strcmp_eq.%d\n", cl);
                printf("  inc rdi\n");
                printf("  inc rsi\n");
                printf("  jmp .L.strcmp_loop.%d\n", cl);
                printf(".L.strcmp_diff.%d:\n", cl);
                printf("  movzx eax, al\n");
                printf("  movzx ecx, byte ptr [rsi]\n");
                printf("  sub eax, ecx\n");
                printf("  jmp .L.strcmp_end.%d\n", cl);
                printf(".L.strcmp_eq.%d:\n", cl);
                printf("  xor eax, eax\n");
                printf(".L.strcmp_end.%d:\n", cl);
                printf(".L.strcmp_end.%d:\n", cl);
                printf("  pop rsi\n");
                printf("  pop rdi\n");
                free_reg(r);
                free_reg(r2);
                int ret = alloc_reg();
                printf("  mov %s, eax\n", reg32[ret]);
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
                printf("  push rdi\n");
                printf("  push rcx\n");
                printf("  mov rdi, %s\n", reg64[sr]);
                printf("  movzx eax, %s\n", reg8[cr]);
                printf(".L.strchr_loop.%d:\n", cl);
                printf("  cmp byte ptr [rdi], al\n");
                printf("  je .L.strchr_found.%d\n", cl);
                printf("  cmp byte ptr [rdi], 0\n");
                printf("  je .L.strchr_null.%d\n", cl);
                printf("  inc rdi\n");
                printf("  jmp .L.strchr_loop.%d\n", cl);
                printf(".L.strchr_found.%d:\n", cl);
                printf("  mov rax, rdi\n");
                printf("  jmp .L.strchr_end.%d\n", cl);
                printf(".L.strchr_null.%d:\n", cl);
                printf("  xor eax, eax\n");
                printf(".L.strchr_end.%d:\n", cl);
                printf("  pop rcx\n");
                printf("  pop rdi\n");
                free_reg(sr);
                free_reg(cr);
                int ret = alloc_reg();
                printf("  mov %s, rax\n", reg64[ret]);
                return ret;
            }
        }
    }

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
        bool is_named = (i < named_count);
        if (arg_is_float[i]) {
            if (is_variadic && !is_named) {
#if defined(__APPLE__)
                // Apple ARM64: variadic args always on the stack
                arg_stack_idx[i] = stack_args++;
#else
                // Linux AAPCS64: variadic floats in FP regs and GP copy
                if (fp_reg_args < max_fp_args) {
                    arg_fp_idx[i] = fp_reg_args++;
                    if (gp_reg_args < max_gp_args)
                        arg_gp_idx[i] = gp_reg_args++;
                } else if (gp_reg_args < max_gp_args) {
                    arg_gp_idx[i] = gp_reg_args++;
                } else {
                    arg_stack_idx[i] = stack_args++;
                }
#endif
            } else if (fp_reg_args < max_fp_args) {
                arg_fp_idx[i] = fp_reg_args++;
                if (is_variadic && gp_reg_args < max_gp_args)
                    arg_gp_idx[i] = gp_reg_args++;
            } else if (is_variadic && arg_sizes[i] > 8) {
                arg_stack_idx[i] = stack_args++;
            } else if (gp_reg_args < max_gp_args) {
                arg_gp_idx[i] = gp_reg_args++;
            } else {
                arg_stack_idx[i] = stack_args++;
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
    int stack_pad = (stack_args & 1) ? 8 : 0;
    int stack_reserve = stack_args > 0 ? stack_args * 8 + stack_pad : 0;
#else
    int gp_reg_args = has_hidden_retbuf ? 1 : 0;
    int fp_reg_args = 0;
    int stack_args = 0;
    for (int i = 0; i < nargs; i++) {
        arg_regs[i] = -1;
        arg_sizes[i] = (argv[i]->ty->kind == TY_ARRAY) ? 8 : argv[i]->ty->size;
        arg_is_float[i] = is_flonum(argv[i]->ty);
        arg_gp_idx[i] = -1;
        arg_fp_idx[i] = -1;
        arg_stack_idx[i] = -1;

        if (arg_is_float[i]) {
            if (argv[i]->ty->kind == TY_LDOUBLE) {
                // long double always on stack as 16 bytes (x87 80-bit + padding)
                if (stack_args & 1) stack_args++; // 16-byte align
                arg_stack_idx[i] = stack_args;
                stack_args += 2;
            } else if (fp_reg_args < max_fp_args) {
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
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            arg_regs[i] = gen_addr(argv[i]);
        else
            arg_regs[i] = gen(argv[i]);
    }

    // Save any live caller-saved registers (indices 0-5: x10-x15) before stack reserve
    int arm64_saved_mask = used_regs & 63;

    if (stack_reserve > 0)
        printf("  sub %s, %s, #%d\n", STACK_REG, STACK_REG, stack_reserve);

    // Save caller-saved regs to [sp] area (on or after sub sp)
    int sv_off = 0;
    for (int i = 0; i < 6; i++) {
        if (arm64_saved_mask & (1 << i)) {
            printf("  str %s, [%s, #%d]\n", reg64[i], STACK_REG, sv_off * 8);
            sv_off++;
        }
    }

    // Push stack args (reverse order)
    for (int i = nargs - 1; i >= 0; i--) {
        if (arg_stack_idx[i] < 0)
            continue;
        int r;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            r = gen_addr(argv[i]);
        else
            r = gen(argv[i]);
        if (is_flonum(argv[i]->ty)) {
            printf("  str %s, [%s, #%d]\n", reg64[r], STACK_REG, arg_stack_idx[i] * 8);
        } else {
            printf("  str %s, [%s, #%d]\n", reg64[r], STACK_REG, arg_stack_idx[i] * 8);
        }
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
                printf("  add %s, %s, #%d\n", reg64[temp_ret_reg], FRAME_PTR, -temp_ret_slot);
            else {
                int v = temp_ret_slot;
                printf("  mov x16, #%d\n", v & 0xffff);
                v >>= 16;
                int s = 16;
                while (v) {
                    printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                    v >>= 16;
                    s += 16;
                }
                printf("  sub %s, %s, x16\n", reg64[temp_ret_reg], FRAME_PTR);
            }
            hidden_ret_reg = temp_ret_reg;
        }
        printf("  mov x8, %s\n", reg64[hidden_ret_reg]);
    }

    // Move evaluated args into arg registers
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if (arg_is_float[i] && arg_fp_idx[i] >= 0) {
            printf("  fmov %s, %s\n", argxmm[arg_fp_idx[i]], reg64[arg_regs[i]]);
            if (arg_gp_idx[i] >= 0)
                printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg64[arg_regs[i]]);
        } else if (arg_is_float[i] && arg_gp_idx[i] >= 0) {
            // Variadic: float value (double bit pattern in GP reg) to GP arg reg
            printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg64[arg_regs[i]]);
        } else if (arg_sizes[i] == 1 || arg_sizes[i] == 2) {
            // For small types, use 64-bit move (caller extends)
            printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg64[arg_regs[i]]);
        } else if (arg_sizes[i] == 4) {
            if (argv[i]->ty->is_unsigned)
                printf("  mov %s, %s\n", argreg32[arg_gp_idx[i]], reg32[arg_regs[i]]);
            else
                printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg64[arg_regs[i]]);
        } else {
            printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg64[arg_regs[i]]);
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
        printf("  blr %s\n", reg64[callee]);
        free_reg(callee);
    }

    if (stack_reserve > 0)
        printf("  add %s, %s, #%d\n", STACK_REG, STACK_REG, stack_reserve);

    // Restore caller-saved registers (saved in sub sp area)
    if (arm64_saved_mask) {
        int sv = 0;
        for (int i = 0; i < 6; i++) {
            if (arm64_saved_mask & (1 << i)) {
                printf("  ldr %s, [%s, #%d]\n", reg64[i], STACK_REG, sv * 8);
                sv++;
            }
        }
    }

    if (has_hidden_retbuf) {
        if (temp_ret_reg != -1) {
            if (temp_ret_slot <= 4095)
                printf("  add %s, %s, #%d\n", reg64[temp_ret_reg], FRAME_PTR, -temp_ret_slot);
            else {
                int v = temp_ret_slot;
                printf("  mov x16, #%d\n", v & 0xffff);
                v >>= 16;
                int s = 16;
                while (v) {
                    printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                    v >>= 16;
                    s += 16;
                }
                printf("  sub %s, %s, x16\n", reg64[temp_ret_reg], FRAME_PTR);
            }
        }
        return temp_ret_reg != -1 ? temp_ret_reg : hidden_ret_reg;
    }

    int r = alloc_reg();
    if (node->ty && is_flonum(node->ty)) {
        printf("  fmov %s, d0\n", reg64[r]);
    } else {
        printf("  mov %s, x0\n", reg64[r]);
    }
    return r;
#else
    // === x86_64 (Windows + Linux) calling convention ===
    int saved_scratch = used_regs & 3;
    if ((saved_scratch & 1) && hidden_ret_reg != 0) {
        printf("  mov [rbp-%d], r10\n", SPILL_R10);
        // Keep r10 marked as in-use so alloc_reg() doesn't reuse it for the
        // hidden ret buffer, which would overwrite a caller's live arg value.
    }
    if ((saved_scratch & 2) && hidden_ret_reg != 1) {
        printf("  mov [rbp-%d], r11\n", SPILL_R11);
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
        printf("  sub rsp, %d\n", stack_reserve);

    for (int i = nargs - 1; i >= reg_nargs; i--) {
        int r = gen(argv[i]);
        if (is_flonum(argv[i]->ty)) {
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + (i - reg_nargs) * 8, reg64[r]);
        } else {
            if (argv[i]->ty->size == 1)
                printf("  movzx %s, %s\n", reg64[r], reg8[r]);
            else if (argv[i]->ty->size == 4)
                printf("  mov %s, %s\n", reg32[r], reg32[r]);
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + (i - reg_nargs) * 8, reg64[r]);
        }
        free_reg(r);
    }
#else
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            arg_regs[i] = gen_addr(argv[i]);
        else
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
        printf("  sub rsp, %d\n", stack_reserve);

    for (int i = nargs - 1; i >= 0; i--) {
        if (arg_stack_idx[i] < 0)
            continue;
        used_regs |= reg_arg_mask; // keep pre-computed reg args live
        // x87: long double args go on stack as 80-bit extended precision
        if (argv[i]->ty->kind == TY_LDOUBLE) {
            int r = gen(argv[i]);
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + arg_stack_idx[i] * 8, reg64[r]);
            printf("  fld qword ptr [rsp+%d]\n", shadow_space + arg_stack_idx[i] * 8);
            printf("  fstp tbyte ptr [rsp+%d]\n", shadow_space + arg_stack_idx[i] * 8);
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
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + arg_stack_idx[i] * 8, reg64[r]);
        } else {
            if (argv[i]->ty->size == 1) {
                if (argv[i]->ty->is_unsigned)
                    printf("  movzx %s, %s\n", reg64[r], reg8[r]);
                else
                    printf("  movsx %s, %s\n", reg32[r], reg8[r]);
            } else if (argv[i]->ty->size == 2) {
                if (argv[i]->ty->is_unsigned)
                    printf("  movzx %s, %s\n", reg64[r], reg16[r]);
                else
                    printf("  movsx %s, %s\n", reg32[r], reg16[r]);
            } else if (argv[i]->ty->size == 4) {
                if (argv[i]->ty->is_unsigned)
                    printf("  mov %s, %s\n", reg32[r], reg32[r]);
                else
                    printf("  movsxd %s, %s\n", reg64[r], reg32[r]);
            }
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + arg_stack_idx[i] * 8, reg64[r]);
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
            printf("  lea %s, [rbp-%d]\n", reg64[temp_ret_reg], temp_ret_slot);
            hidden_ret_reg = temp_ret_reg;
        }
        printf("  mov %s, %s\n", argreg64[0], reg64[hidden_ret_reg]);
    }

#ifdef _WIN32
    for (int i = 0; i < reg_nargs; i++) {
        int argi = i + (has_hidden_retbuf ? 1 : 0);
        if (arg_is_float[i]) {
            printf("  movq %s, %s\n", argxmm[argi], reg64[arg_regs[i]]);
            printf("  mov %s, %s\n", argreg64[argi], reg64[arg_regs[i]]);
        } else if (arg_sizes[i] == 1) {
            if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                printf("  movsx %s, %s\n", argreg32[argi], reg8[arg_regs[i]]);
            else
                printf("  movzx %s, %s\n", argreg64[argi], reg8[arg_regs[i]]);
        } else if (arg_sizes[i] == 2) {
            if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                printf("  movsx %s, %s\n", argreg32[argi], reg16[arg_regs[i]]);
            else
                printf("  movzx %s, %s\n", argreg64[argi], reg16[arg_regs[i]]);
        } else if (arg_sizes[i] == 4) {
            if (argv[i]->ty->is_unsigned)
                printf("  mov %s, %s\n", argreg32[argi], reg(arg_regs[i], 4));
            else
                printf("  movsxd %s, %s\n", argreg64[argi], reg(arg_regs[i], 4));
        } else {
            printf("  mov %s, %s\n", argreg64[argi], reg(arg_regs[i], 8));
        }
        free_reg(arg_regs[i]);
    }
#else
    // Two-pass placement: reg64[7]=="rsi"==argreg64[1], so any arg whose scratch
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
                printf("  movq %s, %s\n", argxmm[arg_fp_idx[i]], reg64[arg_regs[i]]);
            } else if (arg_sizes[i] == 1) {
                if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                    printf("  movsx %s, %s\n", argreg32[arg_gp_idx[i]], reg8[arg_regs[i]]);
                else
                    printf("  movzx %s, %s\n", argreg64[arg_gp_idx[i]], reg8[arg_regs[i]]);
            } else if (arg_sizes[i] == 2) {
                if (argv[i]->ty && !argv[i]->ty->is_unsigned)
                    printf("  movsx %s, %s\n", argreg32[arg_gp_idx[i]], reg16[arg_regs[i]]);
                else
                    printf("  movzx %s, %s\n", argreg64[arg_gp_idx[i]], reg16[arg_regs[i]]);
            } else if (arg_sizes[i] == 4) {
                if (argv[i]->ty->is_unsigned)
                    printf("  mov %s, %s\n", argreg32[arg_gp_idx[i]], reg(arg_regs[i], 4));
                else
                    printf("  movsxd %s, %s\n", argreg64[arg_gp_idx[i]], reg(arg_regs[i], 4));
            } else {
                printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg(arg_regs[i], 8));
            }
            free_reg(arg_regs[i]);
        }
    } // end two-pass
#endif

    printf("  mov eax, %d\n", xmm_args);
    if (call_target) {
        if (strcmp(call_target, "alloca") == 0) {
            alloca_needed = true;
            fn_uses_alloca = true;
            emit_direct_call("__rcc_alloca");
        } else {
            emit_direct_call(call_target);
        }
    } else {
        printf("  call %s\n", reg64[callee_reg]);
        free_reg(callee_reg);
    }

    if (stack_reserve > 0)
        printf("  add rsp, %d\n", stack_reserve);

    if ((saved_scratch & 2) && hidden_ret_reg != 1) {
        used_regs |= 2;
        printf("  mov r11, [rbp-%d]\n", SPILL_R11);
    }
    if ((saved_scratch & 1) && hidden_ret_reg != 0) {
        used_regs |= 1;
        printf("  mov r10, [rbp-%d]\n", SPILL_R10);
    }

    if (has_hidden_retbuf) {
        if (temp_ret_reg != -1) {
            // temp_ret_reg (r10/r11) may have been clobbered by the callee; reload
            // the frame-relative address — it is always valid.
            printf("  lea %s, [rbp-%d]\n", reg64[temp_ret_reg], temp_ret_slot);
        }
        return temp_ret_reg != -1 ? temp_ret_reg : hidden_ret_reg;
    }

    int r = alloc_reg();
    if (node->ty && is_flonum(node->ty)) {
        printf("  movq %s, xmm0\n", reg64[r]);
    } else {
        printf("  mov %s, rax\n", reg64[r]);
    }
    return r;
#endif
}

// Map any register name to a physical register ID (for peephole optimization)
static int phys_reg_id(const char *s) {
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
    return -1;
}
static int same_phys(const char *a, const char *b) {
    int ia = phys_reg_id(a), ib = phys_reg_id(b);
    return ia >= 0 && ia == ib;
}

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
        "al", "ah", "ax", "eax", "rax",
        "bl", "bh", "bx", "ebx", "rbx",
        "cl", "ch", "cx", "ecx", "rcx",
        "dl", "dh", "dx", "edx", "rdx",
        "sil", "si", "esi", "rsi",
        "dil", "di", "edi", "rdi",
        "bpl", "bp", "ebp", "rbp",
        "spl", "sp", "esp", "rsp",
        "r8", "r8b", "r8w", "r8d",
        "r9", "r9b", "r9w", "r9d",
        "r10", "r10b", "r10w", "r10d",
        "r11", "r11b", "r11w", "r11d",
        "r12", "r12b", "r12w", "r12d",
        "r13", "r13b", "r13w", "r13d",
        "r14", "r14b", "r14w", "r14d",
        "r15", "r15b", "r15w", "r15d",
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
    if (r < 0 || r >= NUM_REGS)
        error("invalid register %d", r);
    if (size == 8) return reg64[r];
    return reg32[r]; // arm64: wN for 1/2/4 bytes
#else
    if (r < 0 || r > 7)
        error("invalid register %d", r);
    if (size == 1) return reg8[r];
    if (size == 2) return reg16[r];
    if (size == 4) return reg32[r];
    return reg64[r];
#endif
}

#ifdef ARCH_ARM64
// Emit mov reg, #imm64 handling any size (movz + movk)
static void emit_mov_imm64(const char *reg, uint64_t val) {
    printf("  mov %s, #%llu\n", reg, val & 0xffffULL);
    val >>= 16;
    int shift = 16;
    while (val) {
        printf("  movk %s, #%llu, lsl #%d\n", reg, val & 0xffffULL, shift);
        val >>= 16;
        shift += 16;
    }
}

// Emit adrp+add pair for label address, with platform-appropriate syntax
// Linux: adrp reg, label / add reg, reg, :lo12:label
// Darwin: adrp reg, label@PAGE / add reg, reg, label@PAGEOFF
static void emit_adrp_add(const char *reg, const char *label) {
#if defined(__APPLE__)
    printf("  adrp %s, %s@PAGE\n", reg, label);
    printf("  add %s, %s, %s@PAGEOFF\n", reg, reg, label);
#else
    printf("  adrp %s, %s\n", reg, label);
    printf("  add %s, %s, :lo12:%s\n", reg, reg, label);
#endif
}

// Emit load/store-safe address for [x29, #-offset] when offset > 255
// Returns register holding the address (must be freed by caller)
static int emit_stack_addr(int offset) {
    int ta = alloc_reg();
    if (offset <= 4095)
        printf("  sub %s, %s, #%d\n", reg64[ta], FRAME_PTR, offset);
    else {
        int v = offset;
        printf("  mov %s, #%d\n", reg64[ta], v & 0xffff);
        v >>= 16;
        int s = 16;
        while (v) {
            printf("  movk %s, #%d, lsl #%d\n", reg64[ta], v & 0xffff, s);
            v >>= 16;
            s += 16;
        }
        printf("  sub %s, %s, %s\n", reg64[ta], FRAME_PTR, reg64[ta]);
    }
    return ta;
}
#endif

static char *ptr_size(int size) {
#ifdef ARCH_ARM64
    // arm64: no ptr suffix needed in load/store (size encoded in register name)
    return "";
#else
    if (size == 1) return "byte ptr";
    if (size == 2) return "word ptr";
    if (size == 4) return "dword ptr";
    return "qword ptr";
#endif
}

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

static void emit_load(Type *ty, int r, char *addr) {
    int load_sz = op_size(ty);
#ifdef ARCH_ARM64
    // ARM64 narrow loads always write to w register (32-bit), zero-extending
    // Handle negative offsets with large magnitude (ARM64 ldr/str 9-bit signed limit)
    int off = 0;
    char base[32];
    char load_op[16];
    char *dst_reg;
    bool needs_split = false;
    if (sscanf(addr, "[%31[^,], #%d]", base, &off) == 2 && off < -255) {
        needs_split = true;
        int tr = alloc_reg();
        if (-off <= 4095)
            printf("  sub %s, %s, #%d\n", reg64[tr], base, -off);
        else {
            int v = -off;
            printf("  mov %s, #%d\n", reg64[tr], v & 0xffff);
            v >>= 16;
            int s = 16;
            while (v) {
                printf("  movk %s, #%d, lsl #%d\n", reg64[tr], v & 0xffff, s);
                v >>= 16;
                s += 16;
            }
            printf("  sub %s, %s, %s\n", reg64[tr], base, reg64[tr]);
        }
        if (ty->size == 1) {
            printf("  %s %s, [%s]\n", ty->is_unsigned ? "ldrb" : "ldrsb", reg(r, 4), reg64[tr]);
        } else if (ty->size == 2) {
            printf("  %s %s, [%s]\n", ty->is_unsigned ? "ldrh" : "ldrsh", reg(r, 4), reg64[tr]);
        } else if (ty->size == 4) {
            printf("  ldr %s, [%s]\n", reg(r, 4), reg64[tr]);
        } else {
            printf("  ldr %s, [%s]\n", reg(r, 8), reg64[tr]);
        }
        free_reg(tr);
        return;
    }
    if (ty->size == 1) {
        printf("  %s %s, %s\n", ty->is_unsigned ? "ldrb" : "ldrsb", reg(r, 4), addr);
        return;
    }
    if (ty->size == 2) {
        printf("  %s %s, %s\n", ty->is_unsigned ? "ldrh" : "ldrsh", reg(r, 4), addr);
        return;
    }
    if (ty->size == 4) {
        printf("  ldr %s, %s\n", reg(r, 4), addr);
        return;
    }
    printf("  ldr %s, %s\n", reg(r, 8), addr);
#else
    if (load_sz < 4) load_sz = 4; // movsx/movzx dest must be wider than source
    if (ty->size == 1) {
        printf("  %s %s, byte ptr %s\n", ty->is_unsigned ? "movzx" : "movsx", reg(r, load_sz), addr);
        return;
    }
    if (ty->size == 2) {
        printf("  %s %s, word ptr %s\n", ty->is_unsigned ? "movzx" : "movsx", reg(r, load_sz), addr);
        return;
    }
    if (ty->size == 4) {
        printf("  mov %s, dword ptr %s\n", reg(r, 4), addr);
        return;
    }
    printf("  mov %s, qword ptr %s\n", reg(r, 8), addr);
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
            if (opt_W)
                fprintf(stderr, "\033[1;33mwarning:\033[0m spilling %s to stack in %s\n", reg64[i], current_fn);
#ifdef ARCH_ARM64
            printf("  str %s, [%s, #-%d]\n", reg64[i], FRAME_PTR, spill_offset(i));
#else
            printf("  mov [rbp-%d], %s\n", spill_offset(i), reg64[i]);
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
        printf("  ldr %s, [%s, #-%d]\n", reg64[i], FRAME_PTR, spill_offset(i));
#else
        printf("  mov %s, [rbp-%d]\n", reg64[i], spill_offset(i));
#endif
        spilled_regs &= ~(1 << i);
    }
    used_regs &= ~(1 << i);
}

static int gen(Node *node);

// Generate code to compute the absolute address of an lvalue.
static int gen_addr(Node *node) {
    switch (node->kind) {
    case ND_LVAR: {
        int r = alloc_reg();
        if (node->var->is_local) {
            if (node->var->ty->kind == TY_VLA) {
#ifdef ARCH_ARM64
                printf("  sub %s, %s, #%d\n", reg64[r], FRAME_PTR, node->var->offset - 8);
#else
                printf("  lea %s, [rbp-%d]\n", reg64[r], node->var->offset - 8);
#endif
            } else {
#ifdef ARCH_ARM64
                if (node->var->offset <= 4095)
                    printf("  sub %s, %s, #%d\n", reg64[r], FRAME_PTR, node->var->offset);
                else {
                    int v = node->var->offset;
                    printf("  mov %s, #%d\n", reg64[r], v & 0xffff);
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        printf("  movk %s, #%d, lsl #%d\n", reg64[r], v & 0xffff, s);
                        v >>= 16;
                        s += 16;
                    }
                    printf("  sub %s, %s, %s\n", reg64[r], FRAME_PTR, reg64[r]);
                }
#else
                printf("  lea %s, [rbp-%d]\n", reg64[r], node->var->offset);
#endif
            }
        } else {
            if (node->var->is_weak)
                printf(".weak %s\n", sym_name(var_label(node->var)));
#ifdef ARCH_ARM64
            emit_adrp_add(reg64[r], sym_name(var_label(node->var)));
#else
            printf("  lea %s, [rip + %s]\n", reg64[r], var_label(node->var));
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
            printf("  add %s, %s, #%d\n", reg64[r], reg64[r], node->member->offset);
        else if (node->member->offset > 0) {
            int ti = alloc_reg();
            printf("  mov %s, #%d\n", reg64[ti], node->member->offset);
            printf("  add %s, %s, %s\n", reg64[r], reg64[r], reg64[ti]);
            free_reg(ti);
        } else if (node->member->offset < 0 && -node->member->offset <= 4095)
            printf("  sub %s, %s, #%d\n", reg64[r], reg64[r], -node->member->offset);
        else if (node->member->offset < 0) {
            int ti = alloc_reg();
            printf("  mov %s, #%d\n", reg64[ti], -node->member->offset);
            printf("  sub %s, %s, %s\n", reg64[r], reg64[r], reg64[ti]);
            free_reg(ti);
        }
#else
        printf("  add %s, %d\n", reg64[r], node->member->offset);
#endif
        return r;
    }
    case ND_COND: {
        // Struct/union ternary lvalue: (cond ? a : b).member
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int cond = gen(node->cond);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(cond, node->cond->ty->size));
        printf("  b.eq .L.else.%d\n", c);
        free_reg(cond);
        int then_r = gen_addr(node->then);
        printf("  mov %s, %s\n", reg64[r], reg64[then_r]);
        free_reg(then_r);
        printf("  b .L.end.%d\n", c);
        printf(".L.else.%d:\n", c);
        int else_r = gen_addr(node->els);
        printf("  mov %s, %s\n", reg64[r], reg64[else_r]);
        free_reg(else_r);
        printf(".L.end.%d:\n", c);
#else
        printf("  cmp %s, 0\n", reg(cond, node->cond->ty->size));
        printf("  je .L.else.%d\n", c);
        free_reg(cond);
        int then_r = gen_addr(node->then);
        printf("  mov %s, %s\n", reg64[r], reg64[then_r]);
        free_reg(then_r);
        printf("  jmp .L.end.%d\n", c);
        printf(".L.else.%d:\n", c);
        int else_r = gen_addr(node->els);
        printf("  mov %s, %s\n", reg64[r], reg64[else_r]);
        free_reg(else_r);
        printf(".L.end.%d:\n", c);
#endif
        return r;
    }
    case ND_CAST:
        // Struct/union cast: treat as address of the inner expression
        if (node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION || node->ty->kind == TY_ARRAY))
            return gen_addr(node->lhs);
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
    default:
        error_tok(node->tok, "lvalue required as left operand of assignment");
        return -1;
    }
}


static void gen_cond_branch_inv(Node *cond, char *label) {
    if (cond->kind == ND_EQ || cond->kind == ND_NE || cond->kind == ND_LT || cond->kind == ND_LE) {
        int r_lhs = gen(cond->lhs);
        int sz = op_size(cond->lhs->ty);
        if (sz < op_size(cond->rhs->ty))
            sz = op_size(cond->rhs->ty);
        if (cond->rhs->kind == ND_NUM && cond->rhs->val == (int32_t)cond->rhs->val) {
#ifdef ARCH_ARM64
            printf("  cmp %s, #%d\n", reg(r_lhs, sz), (int)cond->rhs->val);
#else
            printf("  cmp %s, %d\n", reg(r_lhs, sz), (int)cond->rhs->val);
#endif
        } else {
            int r_rhs = gen(cond->rhs);
            printf("  cmp %s, %s\n", reg(r_lhs, sz), reg(r_rhs, sz));
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

        printf("  %s %s\n", jmp, label);
        return;
    }

    int r = gen(cond);
#ifdef ARCH_ARM64
    printf("  cmp %s, #0\n", reg(r, cond->ty->size));
    free_reg(r);
    printf("  b.eq %s\n", label);
#else
    printf("  cmp %s, 0\n", reg(r, cond->ty->size));
    free_reg(r);
    printf("  je %s\n", label);
#endif
}

// Generate code for a given node.
static int gen(Node *node) {
    if (!node) return -1;

    switch (node->kind) {
    case ND_NUM: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        uint64_t v = (uint64_t)(long long)node->val;
        // Use x register for 64-bit immediates, w for 32-bit (stops at lsl #16)
        if (op_size(node->ty) == 4) {
            printf("  mov %s, #%llu\n", reg32[r], v & 0xffff);
            v >>= 16;
            if (v) {
                printf("  movk %s, #%llu, lsl #16\n", reg32[r], v & 0xffff);
            }
        } else {
            printf("  mov %s, #%llu\n", reg64[r], v & 0xffff);
            v >>= 16;
            int shift = 16;
            while (v) {
                printf("  movk %s, #%llu, lsl #%d\n", reg64[r], v & 0xffff, shift);
                v >>= 16;
                shift += 16;
            }
        }
#else
        if (node->val == (int32_t)node->val) {
            printf("  mov %s, %lld\n", reg(r, op_size(node->ty)), (long long)node->val);
        } else {
            printf("  movabs %s, %lld\n", reg64[r], (long long)node->val);
        }
#endif
        return r;
    }
    case ND_FNUM: {
        int r = alloc_reg();
        int id = add_float_literal(node->fval, 8); // Always store as double
#ifdef ARCH_ARM64
        emit_adrp_add("x11", format(".LF%d", id));
        printf("  ldr d0, [x11]\n");
        printf("  fmov %s, d0\n", reg64[r]);
#else
        printf("  movsd xmm0, [rip + .LF%d]\n", id);
        printf("  movq %s, xmm0\n", reg64[r]);
#endif
        return r;
    }
    case ND_LVAR: {
        int r = alloc_reg();
        char *label = var_label(node->var);
        if (node->var->ty->kind == TY_VLA) {
            if (node->var->is_local)
                printf("  mov %s, [rbp-%d]\n", reg64[r], node->var->offset - 8);
            else
                printf("  mov %s, [rip + %s]\n", reg64[r], label);
        } else if (node->var->ty->kind == TY_ARRAY) {
            if (node->var->is_local)
#ifdef ARCH_ARM64
                if (node->var->offset <= 4095)
                    printf("  sub %s, %s, #%d\n", reg64[r], FRAME_PTR, node->var->offset);
                else {
                    int v = node->var->offset;
                    printf("  mov %s, #%d\n", reg64[r], v & 0xffff);
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        printf("  movk %s, #%d, lsl #%d\n", reg64[r], v & 0xffff, s);
                        v >>= 16;
                        s += 16;
                    }
                    printf("  sub %s, %s, %s\n", reg64[r], FRAME_PTR, reg64[r]);
                }
#else
                printf("  lea %s, [rbp-%d]\n", reg64[r], node->var->offset);
#endif
            else
#ifdef ARCH_ARM64
                emit_adrp_add(reg64[r], label);
#else
                printf("  lea %s, [rip + %s]\n", reg64[r], label);
#endif
        } else if (!node->var->is_local && node->var->is_function) {
            if (node->var->is_weak)
                printf(".weak %s\n", sym_name(label));
#ifdef ARCH_ARM64
            emit_adrp_add(reg64[r], sym_name(label));
#else
            printf("  lea %s, [rip + %s]\n", reg64[r], label);
#endif
        } else if (is_flonum(node->var->ty)) {
            {
                if (node->var->is_local) {
                    if (node->var->ty->size == 4) {
#ifdef ARCH_ARM64
                        printf("  ldr s0, [%s, #-%d]\n", FRAME_PTR, node->var->offset);
                        printf("  fcvt d0, s0\n");
#else
                        printf("  movss xmm0, dword ptr [rbp-%d]\n", node->var->offset);
                        printf("  cvtss2sd xmm0, xmm0\n");
#endif
                    } else {
#ifdef ARCH_ARM64
                        printf("  ldr d0, [%s, #-%d]\n", FRAME_PTR, node->var->offset);
#else
                        printf("  movsd xmm0, qword ptr [rbp-%d]\n", node->var->offset);
#endif
                    }
                } else {
                    if (node->var->ty->size == 4) {
#ifdef ARCH_ARM64
                        emit_adrp_add("x11", label);
                        printf("  ldr s0, [x11]\n");
                        printf("  fcvt d0, s0\n");
#else
                        printf("  movss xmm0, dword ptr [rip + %s]\n", label);
                        printf("  cvtss2sd xmm0, xmm0\n");
#endif
                    } else {
#ifdef ARCH_ARM64
                        emit_adrp_add("x11", label);
                        printf("  ldr d0, [x11]\n");
#else
                        printf("  movsd xmm0, qword ptr [rip + %s]\n", label);
#endif
                    }
                }
#ifdef ARCH_ARM64
                printf("  fmov %s, d0\n", reg64[r]);
#else
                printf("  movq %s, xmm0\n", reg64[r]);
#endif
            }
        } else {
            if (node->var->is_local)
#ifdef ARCH_ARM64
                emit_load(node->ty, r, format("[%s, #-%d]", FRAME_PTR, node->var->offset));
#else
                emit_load(node->ty, r, format("[rbp-%d]", node->var->offset));
#endif
            else {
#ifdef ARCH_ARM64
                // Global variable: load address via ADRP+ADD, then deref
                int ta = alloc_reg();
                emit_adrp_add(reg64[ta], label);
                emit_load(node->ty, r, format("[%s]", reg64[ta]));
                free_reg(ta);
#else
                emit_load(node->ty, r, format("[rip + %s]", label));
#endif
            }
        }
        return r;
    }
    case ND_ASSIGN: {
        if (node->lhs->ty->kind == TY_ARRAY || node->lhs->ty->kind == TY_STRUCT || node->lhs->ty->kind == TY_UNION) {
            int c = ++rcc_label_count;
            int dst = gen_addr(node->lhs);
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
                    printf("  mov x12, #%d\n", copy_len);
                    printf(".L.copy.%d:\n", c);
                    printf("  cmp x12, #0\n");
                    printf("  b.eq .L.copy_end.%d\n", c);
                    printf("  sub x12, x12, #1\n");
                    printf("  ldrb w13, [%s, x12]\n", reg64[src]);
                    printf("  strb w13, [%s, x12]\n", reg64[dst]);
                    printf("  b .L.copy.%d\n", c);
                    printf(".L.copy_end.%d:\n", c);
                }
                if (copy_len < lhs_size) {
                    printf("  mov x12, #%d\n", lhs_size - copy_len);
                    int c2 = ++rcc_label_count;
                    printf(".L.copy.%d:\n", c2);
                    printf("  cmp x12, #0\n");
                    printf("  b.eq .L.copy_end.%d\n", c2);
                    printf("  sub x12, x12, #1\n");
                    printf("  strb wzr, [%s, x12]\n", reg64[dst]);
                    printf("  b .L.copy.%d\n", c2);
                    printf(".L.copy_end.%d:\n", c2);
                }
#else
                    printf("  mov rcx, %d\n", copy_len);
                    printf(".L.copy.%d:\n", c);
                    printf("  cmp rcx, 0\n");
                    printf("  je .L.copy_end.%d\n", c);
                    printf("  mov al, byte ptr [%s + rcx - 1]\n", reg64[src]);
                    printf("  mov byte ptr [%s + rcx - 1], al\n", reg64[dst]);
                    printf("  sub rcx, 1\n");
                    printf("  jmp .L.copy.%d\n", c);
                    printf(".L.copy_end.%d:\n", c);
                }
                if (copy_len < lhs_size) {
                    printf("  mov rcx, %d\n", lhs_size - copy_len);
                    int c2 = ++rcc_label_count;
                    printf(".L.copy.%d:\n", c2);
                    printf("  cmp rcx, 0\n");
                    printf("  je .L.copy_end.%d\n", c2);
                    printf("  mov byte ptr [%s + %d + rcx - 1], 0\n", reg64[dst], copy_len);
                    printf("  sub rcx, 1\n");
                    printf("  jmp .L.copy.%d\n", c2);
                    printf(".L.copy_end.%d:\n", c2);
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
                printf("  str %s, [%s]\n", reg(src, node->lhs->ty->size), reg64[dst]);
#else
                printf("  mov [%s], %s\n", reg64[dst], reg(src, node->lhs->ty->size));
#endif
                free_reg(src);
                return dst;
            }

#ifdef ARCH_ARM64
            printf("  mov x12, #%d\n", node->lhs->ty->size);
            printf(".L.copy.%d:\n", c);
            printf("  cmp x12, #0\n");
            printf("  b.eq .L.copy_end.%d\n", c);
            printf("  sub x12, x12, #1\n");
            printf("  ldrb w13, [%s, x12]\n", reg64[src]);
            printf("  strb w13, [%s, x12]\n", reg64[dst]);
            printf("  b .L.copy.%d\n", c);
            printf(".L.copy_end.%d:\n", c);
#else
            printf("  mov rcx, %d\n", node->lhs->ty->size);
            printf(".L.copy.%d:\n", c);
            printf("  cmp rcx, 0\n");
            printf("  je .L.copy_end.%d\n", c);
            printf("  mov al, byte ptr [%s + rcx - 1]\n", reg64[src]);
            printf("  mov byte ptr [%s + rcx - 1], al\n", reg64[dst]);
            printf("  sub rcx, 1\n");
            printf("  jmp .L.copy.%d\n", c);
            printf(".L.copy_end.%d:\n", c);
#endif
            free_reg(src);
            return dst;
        }
        if (is_flonum(node->lhs->ty)) {
#ifdef ARCH_ARM64
            int r2 = gen(node->rhs);
            int r1 = gen_addr(node->lhs);
            printf("  fmov d0, %s\n", reg64[r2]);
            if (node->lhs->ty->size == 4) {
                printf("  fcvt s0, d0\n");
                printf("  str s0, [%s]\n", reg64[r1]);
            } else {
                printf("  str d0, [%s]\n", reg64[r1]);
            }
            free_reg(r1);
            return r2;
#else
#ifndef _WIN32
            if (node->lhs->ty->kind == TY_LDOUBLE) {
                // Store long double as 64-bit double (truncated)
                int r2 = gen(node->rhs);
                int r1 = gen_addr(node->lhs);
                printf("  movq xmm0, %s\n", reg64[r2]);
                printf("  movsd qword ptr [%s], xmm0\n", reg64[r1]);
                free_reg(r2);
                free_reg(r1);
                int dummy = alloc_reg();
                printf("  mov %s, 0\n", reg64[dummy]);
                return dummy;
            }
#endif
            int r2 = gen(node->rhs);
            int r1 = gen_addr(node->lhs);
            printf("  movq xmm0, %s\n", reg64[r2]);
            if (node->lhs->ty->size == 4) {
                printf("  cvtsd2ss xmm0, xmm0\n");
                printf("  movss dword ptr [%s], xmm0\n", reg64[r1]);
            } else {
                printf("  movsd qword ptr [%s], xmm0\n", reg64[r1]);
            }
            free_reg(r1);
            return r2;
#endif
        }
        if (node->lhs->kind == ND_LVAR && node->lhs->var->is_local && node->lhs->var->ty->kind != TY_ARRAY) {
            int r2 = gen(node->rhs);
#ifdef ARCH_ARM64
            if (node->lhs->var->offset <= 255)
                printf("  str %s, [%s, #-%d]\n", reg(r2, node->lhs->ty->size), FRAME_PTR, node->lhs->var->offset);
            else {
                int ta = emit_stack_addr(node->lhs->var->offset);
                printf("  str %s, [%s]\n", reg(r2, node->lhs->ty->size), reg64[ta]);
                free_reg(ta);
            }
#else
            printf("  mov [rbp-%d], %s\n", node->lhs->var->offset, reg(r2, node->lhs->ty->size));
#endif
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
#define BF_LOAD(sz, ra, rt) do { \
    if ((sz) == 1) printf("  ldrb %s, [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 2) printf("  ldrh %s, [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 4) printf("  ldr %s, [%s]\n", reg32[rt], reg64[ra]); \
    else printf("  ldr %s, [%s]\n", reg64[rt], reg64[ra]); \
} while (0)
#define BF_STORE(sz, ra, rt) do { \
    if ((sz) == 1) printf("  strb %s, [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 2) printf("  strh %s, [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 4) printf("  str %s, [%s]\n", reg32[rt], reg64[ra]); \
    else printf("  str %s, [%s]\n", reg64[rt], reg64[ra]); \
} while (0)
#else
#define BF_LOAD(sz, ra, rt) do { \
    if ((sz) == 1) printf("  movzx %s, byte ptr [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 2) printf("  movzx %s, word ptr [%s]\n", reg32[rt], reg64[ra]); \
    else if ((sz) == 4) printf("  mov %s, dword ptr [%s]\n", reg32[rt], reg64[ra]); \
    else printf("  mov %s, qword ptr [%s]\n", reg64[rt], reg64[ra]); \
} while (0)
#define BF_STORE(sz, ra, rt) do { \
    if ((sz) == 1) printf("  mov byte ptr [%s], %s\n", reg64[ra], reg8[rt]); \
    else if ((sz) == 2) printf("  mov word ptr [%s], %s\n", reg64[ra], reg(rt, 2)); \
    else if ((sz) == 4) printf("  mov dword ptr [%s], %s\n", reg64[ra], reg(rt, 4)); \
    else printf("  mov qword ptr [%s], %s\n", reg64[ra], reg64[rt]); \
} while (0)
#endif

            // Generate RHS (the new value to assign)
            int r2 = gen(node->rhs);

            if (rhs_reads_same) {
                int ra = gen_addr(node->lhs);
                int rt = alloc_reg();
                BF_LOAD(unit_sz, ra, rt);
#ifdef ARCH_ARM64
                if (bo > 0) printf("  lsr %s, %s, #%d\n", reg64[rt], reg64[rt], bo);
                if (bw < unit_sz * 8) {
                    unsigned long long m = (1ULL << bw) - 1;
                    int t2 = alloc_reg();
                    emit_mov_imm64(reg64[t2], m);
                    printf("  and %s, %s, %s\n", reg64[rt], reg64[rt], reg64[t2]);
                    free_reg(t2);
                }
                BF_LOAD(unit_sz, ra, rt);
                int tmp = alloc_reg();
                emit_mov_imm64(reg64[tmp], ~mask);
                printf("  and %s, %s, %s\n", reg64[rt], reg64[rt], reg64[tmp]);
                int rv = alloc_reg();
                printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
                emit_mov_imm64(reg64[tmp], (1ULL << bw) - 1);
                printf("  and %s, %s, %s\n", reg64[rv], reg64[rv], reg64[tmp]);
                if (bo > 0) printf("  lsl %s, %s, #%d\n", reg64[rv], reg64[rv], bo);
                printf("  orr %s, %s, %s\n", reg64[rt], reg64[rt], reg64[rv]);
                BF_STORE(unit_sz, ra, rt);
                free_reg(tmp);
                free_reg(rv);
                free_reg(rt);
                free_reg(ra);
                return r2;
            }

            // Simple assignment: read-modify-write
            free_reg(gen_addr(node->lhs));
            int ra = gen_addr(node->lhs);
            int rt = alloc_reg();
            int eff_sz = unit_sz > 8 ? 8 : unit_sz;
            BF_LOAD(eff_sz, ra, rt);
            int tmp = alloc_reg();
            emit_mov_imm64(reg64[tmp], ~mask);
            printf("  and %s, %s, %s\n", reg64[rt], reg64[rt], reg64[tmp]);
            int rv = alloc_reg();
            printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
            emit_mov_imm64(reg64[tmp], (1ULL << bw) - 1);
            printf("  and %s, %s, %s\n", reg64[rv], reg64[rv], reg64[tmp]);
            if (bo > 0) printf("  lsl %s, %s, #%d\n", reg64[rv], reg64[rv], bo);
            printf("  orr %s, %s, %s\n", reg64[rt], reg64[rt], reg64[rv]);
            BF_STORE(eff_sz, ra, rt);
            if (unit_sz > 8 && bo + bw > 64) {
                int overflow = bo + bw - 64;
                unsigned int ovf_mask = (1u << overflow) - 1;
                printf("  add %s, %s, #8\n", reg64[ra], reg64[ra]);
                printf("  ldrb %s, [%s]\n", reg32[rt], reg64[ra]);
                printf("  and %s, %s, #%u\n", reg32[rt], reg32[rt], (unsigned)(~ovf_mask & 0xFF));
                printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
                printf("  lsr %s, %s, #%d\n", reg64[rv], reg64[rv], 64 - bo);
                printf("  and %s, %s, #%u\n", reg32[rv], reg32[rv], ovf_mask);
                printf("  orr %s, %s, %s\n", reg32[rt], reg32[rt], reg32[rv]);
                printf("  strb %s, [%s]\n", reg32[rt], reg64[ra]);
            }
#else
                if (bo > 0) printf("  shr %s, %d\n", reg64[rt], bo);
                if (bw < unit_sz * 8) {
                    unsigned long long m = (1ULL << bw) - 1;
                    printf("  movabs rax, %llu\n", m);
                    printf("  and %s, rax\n", reg64[rt]);
                }
                // Re-load for modify
                BF_LOAD(unit_sz, ra, rt);
                printf("  movabs rax, %llu\n", ~mask);
                printf("  and %s, rax\n", reg64[rt]);
                int rv = alloc_reg();
                printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
                printf("  movabs rax, %llu\n", (1ULL << bw) - 1);
                printf("  and %s, rax\n", reg64[rv]);
                if (bo > 0) printf("  shl %s, %d\n", reg64[rv], bo);
                printf("  or %s, %s\n", reg64[rt], reg64[rv]);
                BF_STORE(unit_sz, ra, rt);
                free_reg(rv);
                free_reg(rt);
                free_reg(ra);
                return r2;
            }

            // Simple assignment: read-modify-write
            free_reg(gen_addr(node->lhs)); // discard; re-gen below
            int ra = gen_addr(node->lhs);
            int rt = alloc_reg();
            int eff_sz = unit_sz > 8 ? 8 : unit_sz;
            BF_LOAD(eff_sz, ra, rt);
            printf("  movabs rax, %llu\n", ~mask);
            printf("  and %s, rax\n", reg64[rt]);
            int rv = alloc_reg();
            printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
            printf("  movabs rax, %llu\n", (1ULL << bw) - 1);
            printf("  and %s, rax\n", reg64[rv]);
            if (bo > 0) printf("  shl %s, %d\n", reg64[rv], bo);
            printf("  or %s, %s\n", reg64[rt], reg64[rv]);
            BF_STORE(eff_sz, ra, rt);
            // Handle overflow bits beyond the first 8 bytes
            if (unit_sz > 8 && bo + bw > 64) {
                int overflow = bo + bw - 64;
                unsigned int ovf_mask = (1u << overflow) - 1;
                // Read-modify-write the byte at addr+8
                printf("  add %s, 8\n", reg64[ra]);
                printf("  movzx %s, byte ptr [%s]\n", reg32[rt], reg64[ra]);
                printf("  and %s, %u\n", reg32[rt], (unsigned)(~ovf_mask & 0xFF));
                printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
                printf("  shr %s, %d\n", reg64[rv], 64 - bo);
                printf("  and %s, %u\n", reg32[rv], ovf_mask);
                printf("  or %s, %s\n", reg32[rt], reg32[rv]);
                printf("  mov byte ptr [%s], %s\n", reg64[ra], reg8[rt]);
            }
#endif
#undef BF_LOAD
#undef BF_STORE
            free_reg(rv);
            free_reg(rt);
            free_reg(ra);
            return r2;
        }
        int r1 = gen_addr(node->lhs);
        int r2 = gen(node->rhs);
#ifdef ARCH_ARM64
        printf("  str %s, [%s]\n", reg(r2, node->lhs->ty->size), reg64[r1]);
#else
        printf("  mov [%s], %s\n", reg64[r1], reg(r2, node->lhs->ty->size));
#endif
        free_reg(r1);
        return r2;
    }
    case ND_NEG: {
        int r = gen(node->lhs);
        if (is_flonum(node->ty)) {
#ifdef ARCH_ARM64
            printf("  fmov d0, %s\n", reg64[r]);
            printf("  fneg d0, d0\n");
            printf("  fmov %s, d0\n", reg64[r]);
#else
            // Negate float: xor with sign bit
            printf("  movq xmm0, %s\n", reg64[r]);
            // Use pxor with sign bit mask
            printf("  movabs %s, %lld\n", reg64[r], (long long)0x8000000000000000LL);
            printf("  movq xmm1, %s\n", reg64[r]);
            printf("  xorpd xmm0, xmm1\n");
            printf("  movq %s, xmm0\n", reg64[r]);
#endif
        } else {
#ifdef ARCH_ARM64
            printf("  neg %s, %s\n", reg(r, op_size(node->ty)), reg(r, op_size(node->ty)));
#else
            printf("  neg %s\n", reg(r, op_size(node->ty)));
#endif
        }
        return r;
    }
    case ND_NOT: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(r, op_size(node->lhs->ty)));
        printf("  cset %s, eq\n", reg(r, 4));
#else
        printf("  cmp %s, 0\n", reg(r, op_size(node->lhs->ty)));
        printf("  sete al\n");
        printf("  movzx %s, al\n", reg(r, 4));
#endif
        return r;
    }
    case ND_ZERO_INIT: {
        // Zero-fill a local variable's stack memory
        LVar *var = node->lhs->var;
        if (!var || !var->is_local || var->ty->size <= 0) return -1;
        int c = ++rcc_label_count;
#ifdef ARCH_ARM64
        printf("  sub x11, %s, #%d\n", FRAME_PTR, var->offset);
        printf("  mov x12, #%d\n", var->ty->size);
        printf(".L.zero.%d:\n", c);
        printf("  cmp x12, #0\n");
        printf("  b.eq .L.zero_end.%d\n", c);
        printf("  sub x12, x12, #1\n");
        printf("  strb wzr, [x11, x12]\n");
        printf("  b .L.zero.%d\n", c);
        printf(".L.zero_end.%d:\n", c);
#else
        printf("  mov rcx, %d\n", var->ty->size);
        printf(".L.zero.%d:\n", c);
        printf("  cmp rcx, 0\n");
        printf("  je .L.zero_end.%d\n", c);
        printf("  mov byte ptr [rbp - %d + rcx - 1], 0\n", var->offset);
        printf("  sub rcx, 1\n");
        printf("  jmp .L.zero.%d\n", c);
        printf(".L.zero_end.%d:\n", c);
#endif
        return -1;
    }
    case ND_POST_INC:
    case ND_POST_DEC: {
        int r = gen_addr(node->lhs);
        int r2 = alloc_reg();
        int sz = node->lhs->ty->size;
#ifdef ARCH_ARM64
        // Load current value
        printf("  ldr %s, [%s]\n", reg(r2, sz), reg64[r]);
        int delta = 1;
        if (node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY)
            delta = node->lhs->ty->base->size;
        // Update in-place: load into temp, add/sub, store back
        int r3 = alloc_reg();
        printf("  ldr %s, [%s]\n", reg(r3, sz), reg64[r]);
        if (node->kind == ND_POST_INC)
            printf("  add %s, %s, #%d\n", reg(r3, sz), reg(r3, sz), delta);
        else
            printf("  sub %s, %s, #%d\n", reg(r3, sz), reg(r3, sz), delta);
        printf("  str %s, [%s]\n", reg(r3, sz), reg64[r]);
        free_reg(r3);
#else
        printf("  mov %s, %s [%s]\n", reg(r2, sz), ptr_size(sz), reg64[r]);
        int delta = 1;
        if (node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY)
            delta = node->lhs->ty->base->size;
        if (node->kind == ND_POST_INC)
            printf("  add %s [%s], %d\n", ptr_size(sz), reg64[r], delta);
        else
            printf("  sub %s [%s], %d\n", ptr_size(sz), reg64[r], delta);
#endif
        free_reg(r);
        return r2;
    }
    case ND_MEMBER: {
        int r = gen_addr(node);
        if (node->ty->kind == TY_ARRAY) {
            return r; // array decays to pointer
        }
        Type *load_ty = (node->member && node->member->bit_width > 0) ? node->member->ty : node->ty;
        if (is_flonum(load_ty)) {
#ifdef ARCH_ARM64
            if (load_ty->size == 4) {
                printf("  ldr s0, [%s]\n", reg64[r]);
                printf("  fcvt d0, s0\n");
            } else {
                printf("  ldr d0, [%s]\n", reg64[r]);
            }
            printf("  fmov %s, d0\n", reg64[r]);
#else
            if (load_ty->size == 4) {
                printf("  movss xmm0, dword ptr [%s]\n", reg64[r]);
                printf("  cvtss2sd xmm0, xmm0\n");
                printf("  movq %s, xmm0\n", reg64[r]);
            } else {
                printf("  movsd xmm0, qword ptr [%s]\n", reg64[r]);
                printf("  movq %s, xmm0\n", reg64[r]);
            }
#endif
        } else if (node->member && node->member->bit_width > 0 && node->member->bf_load_size) {
            int ls = node->member->bf_load_size;
            int bw = node->member->bit_width;
            int bo = node->member->bit_offset;
            int eff_ls = ls > 8 ? 8 : ls;
            int r_addr = -1;
            if (ls > 8 && bo + bw > 64) {
                r_addr = alloc_reg();
                printf("  mov %s, %s\n", reg64[r_addr], reg64[r]);
            }
#ifdef ARCH_ARM64
            if (eff_ls <= 2)
                printf("  ldrh %s, [%s]\n", reg32[r], reg64[r]);
            else if (eff_ls == 4)
                printf("  ldr %s, [%s]\n", reg32[r], reg64[r]);
            else
                printf("  ldr %s, [%s]\n", reg64[r], reg64[r]);
            if (bo > 0)
                printf("  lsr %s, %s, #%d\n", reg64[r], reg64[r], bo);
            if (r_addr >= 0) {
                int overflow = bo + bw - 64;
                int tmp = alloc_reg();
                printf("  ldrb %s, [%s, #8]\n", reg32[tmp], reg64[r_addr]);
                printf("  and %s, %s, #%d\n", reg32[tmp], reg32[tmp], (1 << overflow) - 1);
                printf("  lsl %s, %s, #%d\n", reg64[tmp], reg64[tmp], 64 - bo);
                printf("  orr %s, %s, %s\n", reg64[r], reg64[r], reg64[tmp]);
                free_reg(tmp);
                free_reg(r_addr);
            }
            int load_bits = ls * 8;
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    int tmp2 = alloc_reg();
                    emit_mov_imm64(reg64[tmp2], mask);
                    printf("  and %s, %s, %s\n", reg64[r], reg64[r], reg64[tmp2]);
                    free_reg(tmp2);
                } else {
                    int shift = 64 - bw;
                    printf("  lsl %s, %s, #%d\n", reg64[r], reg64[r], shift);
                    printf("  asr %s, %s, #%d\n", reg64[r], reg64[r], shift);
                }
            }
            return r;
#else
            const char *sz = eff_ls == 1 ? "byte" : eff_ls == 2 ? "word"
                : eff_ls == 4                                   ? "dword"
                                                                : "qword";
            if (eff_ls <= 2)
                printf("  movzx %s, %s ptr [%s]\n", reg32[r], sz, reg64[r]);
            else if (eff_ls == 4)
                printf("  mov %s, %s ptr [%s]\n", reg32[r], sz, reg64[r]);
            else
                printf("  mov %s, %s ptr [%s]\n", reg64[r], sz, reg64[r]);
            if (bo > 0)
                printf("  shr %s, %d\n", reg64[r], bo);
            if (r_addr >= 0) {
                int overflow = bo + bw - 64;
                int tmp = alloc_reg();
                printf("  movzx %s, byte ptr [%s+8]\n", reg32[tmp], reg64[r_addr]);
                printf("  and %s, %d\n", reg32[tmp], (1 << overflow) - 1);
                printf("  shl %s, %d\n", reg64[tmp], 64 - bo);
                printf("  or %s, %s\n", reg64[r], reg64[tmp]);
                free_reg(tmp);
                free_reg(r_addr);
            }
            int load_bits = ls * 8;
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    printf("  movabs rax, %llu\n", mask);
                    printf("  and %s, rax\n", reg64[r]);
                } else {
                    int shift = 64 - bw;
                    printf("  shl %s, %d\n", reg64[r], shift);
                    printf("  sar %s, %d\n", reg64[r], shift);
                }
            }
            return r;
#endif
        } else {
            emit_load(load_ty, r, format("[%s]", reg64[r]));
        }
        if (node->member && node->member->bit_width > 0) {
            int bw = node->member->bit_width;
            int bo = node->member->bit_offset;
            int load_bits = node->member->bf_load_size
                ? node->member->bf_load_size * 8
                : node->member->ty->size * 8;
#ifdef ARCH_ARM64
            if (bo > 0)
                printf("  lsr %s, %s, #%d\n", reg64[r], reg64[r], bo);
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    int t = alloc_reg();
                    emit_mov_imm64(reg64[t], mask);
                    printf("  and %s, %s, %s\n", reg64[r], reg64[r], reg64[t]);
                    free_reg(t);
                } else {
                    int shift = 64 - bw;
                    printf("  lsl %s, %s, #%d\n", reg64[r], reg64[r], shift);
                    printf("  asr %s, %s, #%d\n", reg64[r], reg64[r], shift);
                }
            }
#else
            if (bo > 0)
                printf("  shr %s, %d\n", reg64[r], bo);
            if (bw < load_bits) {
                if (node->member->ty->is_unsigned || node->member->ty->is_enum) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    printf("  movabs rax, %llu\n", mask);
                    printf("  and %s, rax\n", reg64[r]);
                } else {
                    int shift = 64 - bw;
                    printf("  shl %s, %d\n", reg64[r], shift);
                    printf("  sar %s, %d\n", reg64[r], shift);
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
            printf("  fmov d0, %s\n", reg64[r]);
            if (to->is_unsigned) {
                printf("  fcvtzu %s, d0\n", reg64[r]);
            } else {
                printf("  fcvtzs %s, d0\n", reg64[r]);
            }
#else
            printf("  movq xmm0, %s\n", reg64[r]);
            if (to->size == 8 && to->is_unsigned) {
                int c = ++rcc_label_count;
                printf("  mov rax, 0x43e0000000000000\n");
                printf("  movq xmm1, rax\n");
                printf("  comisd xmm0, xmm1\n");
                printf("  jb .L.ucast.%d\n", c);
                printf("  subsd xmm0, xmm1\n");
                printf("  cvttsd2si %s, xmm0\n", reg64[r]);
                printf("  mov rcx, 0x8000000000000000\n");
                printf("  or %s, rcx\n", reg64[r]);
                printf("  jmp .L.ucast_end.%d\n", c);
                printf(".L.ucast.%d:\n", c);
                printf("  cvttsd2si %s, xmm0\n", reg64[r]);
                printf(".L.ucast_end.%d:\n", c);
            } else {
                printf("  cvttsd2si %s, xmm0\n", to->size == 8 ? reg64[r] : reg32[r]);
            }
#endif
        } else if (is_integer(from) && is_flonum(to)) {
#ifdef ARCH_ARM64
            printf("  scvtf d0, %s\n", from->size < 4 ? reg32[r] : reg(r, from->size));
            if (to->kind == TY_FLOAT) {
                printf("  fcvt s0, d0\n");
                printf("  fcvt d0, s0\n");
            }
            printf("  fmov %s, d0\n", reg64[r]);
#else
            printf("  cvtsi2sd xmm0, %s\n", from->size < 4 ? reg32[r] : reg(r, from->size));
            if (to->kind == TY_FLOAT) {
                printf("  cvtsd2ss xmm0, xmm0\n");
                printf("  cvtss2sd xmm0, xmm0\n");
            }
            printf("  movq %s, xmm0\n", reg64[r]);
#endif
        } else if (is_flonum(from) && is_flonum(to)) {
#ifdef ARCH_ARM64
            if (to->kind == TY_FLOAT && from->kind != TY_FLOAT) {
                printf("  fmov d0, %s\n", reg64[r]);
                printf("  fcvt s0, d0\n");
                printf("  fcvt d0, s0\n");
                printf("  fmov %s, d0\n", reg64[r]);
            }
#else
            if (to->kind == TY_FLOAT && from->kind != TY_FLOAT) {
                printf("  movq xmm0, %s\n", reg64[r]);
                printf("  cvtsd2ss xmm0, xmm0\n");
                printf("  cvtss2sd xmm0, xmm0\n");
                printf("  movq %s, xmm0\n", reg64[r]);
            }
#endif
        } else if (to->size == 1) {
#ifdef ARCH_ARM64
            if (to->is_unsigned)
                printf("  and %s, %s, #0xff\n", reg32[r], reg32[r]);
            else
                printf("  sxtb %s, %s\n", reg32[r], reg32[r]);
#else
            if (to->is_unsigned)
                printf("  movzx %s, %s\n", reg32[r], reg8[r]);
            else
                printf("  movsx %s, %s\n", reg32[r], reg8[r]);
#endif
        } else if (to->size == 2) {
#ifdef ARCH_ARM64
            if (to->is_unsigned)
                printf("  and %s, %s, #0xffff\n", reg32[r], reg32[r]);
            else
                printf("  sxth %s, %s\n", reg32[r], reg32[r]);
#else
            if (to->is_unsigned)
                printf("  movzx %s, %s\n", reg32[r], reg16[r]);
            else
                printf("  movsx %s, %s\n", reg32[r], reg16[r]);
#endif
        } else if (to->size == 4 && from->size == 8) {
            printf("  mov %s, %s\n", reg32[r], reg32[r]);
        } else if (to->size == 8 && from->size < 8) {
#ifdef ARCH_ARM64
            if (from->size == 4) {
                if (from->is_unsigned)
                    printf("  mov %s, %s\n", reg32[r], reg32[r]);
                else
                    printf("  sxtw %s, %s\n", reg64[r], reg32[r]);
            } else {
                if (from->size == 1) {
                    if (from->is_unsigned)
                        printf("  uxtb %s, %s\n", reg64[r], reg32[r]);
                    else
                        printf("  sxtb %s, %s\n", reg64[r], reg32[r]);
                } else {
                    if (from->is_unsigned)
                        printf("  uxth %s, %s\n", reg64[r], reg32[r]);
                    else
                        printf("  sxth %s, %s\n", reg64[r], reg32[r]);
                }
            }
#else
            if (from->size == 4) {
                if (from->is_unsigned)
                    printf("  mov %s, %s\n", reg32[r], reg32[r]);
                else
                    printf("  movsxd %s, %s\n", reg64[r], reg32[r]);
            } else {
                // 8-bit or 16-bit -> 64-bit
                if (from->is_unsigned)
                    printf("  movzx %s, %s\n", reg64[r], reg(r, from->size));
                else
                    printf("  movsx %s, %s\n", reg64[r], reg(r, from->size));
            }
#endif
        }
        return r;
    }
    case ND_BITNOT: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        printf("  mvn %s, %s\n", reg(r, op_size(node->ty)), reg(r, op_size(node->ty)));
#else
        printf("  not %s\n", reg(r, op_size(node->ty)));
#endif
        return r;
    }
    case ND_STR: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        emit_adrp_add(reg64[r], format(".LC%d", node->str_id));
#else
        printf("  lea %s, [rip + .LC%d]\n", reg64[r], node->str_id);
#endif
        return r;
    }
    case ND_DEREF: {
        if (node->ty->kind == TY_FUNC || node->ty->kind == TY_ARRAY)
            return gen(node->lhs);
        int r = gen(node->lhs);
        if (is_flonum(node->ty)) {
#ifdef ARCH_ARM64
            if (node->ty->size == 4) {
                printf("  ldr s0, [%s]\n", reg64[r]);
                printf("  fcvt d0, s0\n");
            } else {
                printf("  ldr d0, [%s]\n", reg64[r]);
            }
            printf("  fmov %s, d0\n", reg64[r]);
#else
            if (node->ty->size == 4) {
                printf("  movss xmm0, dword ptr [%s]\n", reg64[r]);
                printf("  cvtss2sd xmm0, xmm0\n");
            } else {
                printf("  movsd xmm0, qword ptr [%s]\n", reg64[r]);
            }
            printf("  movq %s, xmm0\n", reg64[r]);
#endif
        } else {
            emit_load(node->ty, r, format("[%s]", reg64[r]));
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
                    if (var->name && var->name[0] == '\0') {
                        retbuf_offset = var->offset;
                        break;
                    }
                }
#ifdef ARCH_ARM64
                if (retbuf_offset <= 4095)
                    printf("  ldr x11, [%s, #-%d]\n", FRAME_PTR, retbuf_offset);
                else {
                    int v = retbuf_offset;
                    printf("  mov x16, #%d\n", v & 0xffff);
                    v >>= 16;
                    int s = 16;
                    while (v) {
                        printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                        v >>= 16;
                        s += 16;
                    }
                    printf("  sub x11, %s, x16\n", FRAME_PTR);
                }
                printf("  mov x12, #%d\n", node->lhs->ty->size);
                printf(".L.retcopy.%d:\n", c);
                printf("  cmp x12, #0\n");
                printf("  b.eq .L.retcopy_end.%d\n", c);
                printf("  sub x12, x12, #1\n");
                printf("  ldrb w13, [%s, x12]\n", reg64[src]);
                printf("  strb w13, [x11, x12]\n");
                printf("  b .L.retcopy.%d\n", c);
                printf(".L.retcopy_end.%d:\n", c);
                printf("  mov x0, x11\n");
#else
                printf("  mov r11, [rbp-%d]\n", retbuf_offset);
                printf("  mov rcx, %d\n", node->lhs->ty->size);
                printf(".L.retcopy.%d:\n", c);
                printf("  cmp rcx, 0\n");
                printf("  je .L.retcopy_end.%d\n", c);
                printf("  mov al, byte ptr [%s + rcx - 1]\n", reg64[src]);
                printf("  mov byte ptr [r11 + rcx - 1], al\n");
                printf("  sub rcx, 1\n");
                printf("  jmp .L.retcopy.%d\n", c);
                printf(".L.retcopy_end.%d:\n", c);
                printf("  mov rax, r11\n");
#endif
                free_reg(src);
            } else {
                int r = gen(node->lhs);
                if (node->lhs->ty && is_flonum(node->lhs->ty)) {
#ifdef ARCH_ARM64
                    printf("  fmov d0, %s\n", reg64[r]);
#else
                    printf("  movq xmm0, %s\n", reg64[r]);
#endif
                } else {
#ifdef ARCH_ARM64
                    printf("  mov x0, %s\n", reg64[r]);
#else
                    printf("  mov rax, %s\n", reg64[r]);
#endif
                }
                free_reg(r);
            }
        }
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
#ifdef ARCH_ARM64
        printf("  b .L.return.%s\n", current_fn);
#else
        printf("  jmp .L.return.%s\n", current_fn);
#endif
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
        if (node->kind == ND_ALLOCA_ZINIT && node->lhs && node->lhs->kind == ND_NUM && node->lhs->val == 0) {
            printf("  mov rsp, [rbp-%d]\n", node->var->offset);
            return -1;
        }
        int r = gen(node->lhs);
        printf("  mov [rbp-%d], rsp\n", node->var->offset);
        printf("  mov rax, %s\n", reg64[r]);
        free_reg(r);
        printf("  mov rcx, rsp\n");
        printf("  sub rsp, rax\n");
        int align = 16;
        printf("  and rsp, -%d\n", align);
        printf("  sub rcx, rsp\n");
        if (node->kind == ND_ALLOCA_ZINIT) {
            printf("  pxor xmm0, xmm0\n");
            printf(".L.alloca.zero.%d:\n", rcc_label_count);
            printf("  sub rcx, 16\n");
            printf("  js .L.alloca.done.%d\n", rcc_label_count);
            printf("  movaps [rsp + rcx], xmm0\n");
            printf("  jmp .L.alloca.zero.%d\n", rcc_label_count);
        } else {
            printf(".L.alloca.probe.%d:\n", rcc_label_count);
            printf("  sub rcx, 4096\n");
            printf("  js .L.alloca.done.%d\n", rcc_label_count);
            printf("  or byte ptr [rsp + rcx], 0\n");
            printf("  jmp .L.alloca.probe.%d\n", rcc_label_count);
        }
        printf(".L.alloca.done.%d:\n", rcc_label_count);
        rcc_label_count++;
        if (node->var)
            printf("  mov [rbp-%d], rsp\n", node->var->offset - 8);
        else
            printf("  mov rax, rsp\n");
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
            printf("  mov %s, 0\n", reg(result, op_size(node->ty)));
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
        int r = alloc_reg();
        int lhs = gen(node->lhs);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, #0\n", reg(r, 4));
        printf("  b.eq .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, #0\n", reg(rhs, node->rhs->ty->size));
        printf("  cset %s, ne\n", reg(r, 4));
#else
        printf("  cmp %s, 0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, 0\n", reg(r, 4));
        printf("  je .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, 0\n", reg(rhs, node->rhs->ty->size));
        printf("  setne al\n");
        printf("  movzx %s, al\n", reg(r, 4));
#endif
        free_reg(rhs);
        printf(".L.end.%d:\n", c);
        return r;
    }
    case ND_LOGOR: {
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int lhs = gen(node->lhs);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, #1\n", reg(r, 4));
        printf("  b.ne .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, #0\n", reg(rhs, node->rhs->ty->size));
        printf("  cset %s, ne\n", reg(r, 4));
#else
        printf("  cmp %s, 0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, 1\n", reg(r, 4));
        printf("  jne .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, 0\n", reg(rhs, node->rhs->ty->size));
        printf("  setne al\n");
        printf("  movzx %s, al\n", reg(r, 4));
#endif
        free_reg(rhs);
        printf(".L.end.%d:\n", c);
        return r;
    }
    case ND_COND: {
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int cond = gen(node->cond);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(cond, node->cond->ty->size));
        printf("  b.eq .L.else.%d\n", c);
        free_reg(cond);
        int then_r = gen(node->then);
        printf("  mov %s, %s\n", reg64[r], reg64[then_r]);
        free_reg(then_r);
        printf("  b .L.end.%d\n", c);
        printf(".L.else.%d:\n", c);
        int else_r = gen(node->els);
        printf("  mov %s, %s\n", reg64[r], reg64[else_r]);
        free_reg(else_r);
#else
        printf("  cmp %s, 0\n", reg(cond, node->cond->ty->size));
        printf("  je .L.else.%d\n", c);
        free_reg(cond);
        int then_r = gen(node->then);
        printf("  mov %s, %s\n", reg64[r], reg64[then_r]);
        free_reg(then_r);
        printf("  jmp .L.end.%d\n", c);
        printf(".L.else.%d:\n", c);
        int else_r = gen(node->els);
        printf("  mov %s, %s\n", reg64[r], reg64[else_r]);
        free_reg(else_r);
#endif
        printf(".L.end.%d:\n", c);
        return r;
    }
    case ND_IF: {
        int c = ++rcc_label_count;
        char end_label[32], else_label[32];
        sprintf(end_label, ".L.end.%d", c);
        sprintf(else_label, ".L.else.%d", c);

        if (node->els) {
            gen_cond_branch_inv(node->cond, else_label);
            int r1 = gen(node->then);
            if (r1 != -1) free_reg(r1);
#ifdef ARCH_ARM64
            printf("  b %s\n", end_label);
#else
            printf("  jmp %s\n", end_label);
#endif
            printf("%s:\n", else_label);
            int r2 = gen(node->els);
            if (r2 != -1) free_reg(r2);
            printf("%s:\n", end_label);
        } else {
            gen_cond_branch_inv(node->cond, end_label);
            int r1 = gen(node->then);
            if (r1 != -1) free_reg(r1);
            printf("%s:\n", end_label);
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
        printf("%s:\n", begin_label);
        if (node->cond) {
            gen_cond_branch_inv(node->cond, end_label);
        }
        int r_then = gen(node->then);
        if (r_then != -1) free_reg(r_then);
        printf("%s:\n", cont_label);
        if (node->inc) {
            int r_inc = gen(node->inc);
            if (r_inc != -1) free_reg(r_inc);
        }
#ifdef ARCH_ARM64
        printf("  b %s\n", begin_label);
#else
        printf("  jmp %s\n", begin_label);
#endif
        printf("%s:\n", end_label);
        ctrl_depth--;
        return -1;
    }
    case ND_DO: {
        int c = ++rcc_label_count;
        printf(".L.begin.%d:\n", c);
        break_stack[ctrl_depth] = c;
        continue_stack[ctrl_depth] = c;
        ctrl_depth++;
        int r_then = gen(node->then);
        if (r_then != -1) free_reg(r_then);
        printf(".L.continue.%d:\n", c);
        int r = gen(node->cond);
#ifdef ARCH_ARM64
        printf("  cmp %s, #0\n", reg(r, node->cond->ty->size));
        free_reg(r);
        printf("  b.ne .L.begin.%d\n", c);
#else
        printf("  cmp %s, 0\n", reg(r, node->cond->ty->size));
        free_reg(r);
        printf("  jne .L.begin.%d\n", c);
#endif
        printf(".L.end.%d:\n", c);
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
                    printf("  cmp %s, #%lld\n", reg(cond, sz), (long long)cs->case_val);
                else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_val);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  %s .L.skip.%d\n", is_uns ? "b.lo" : "b.lt", skip_lbl);
                if ((cs->case_end >= 0 && cs->case_end <= 4095) ||
                    (cs->case_end > 0 && cs->case_end <= 0xffffff && (cs->case_end % 4096) == 0))
                    printf("  cmp %s, #%lld\n", reg(cond, sz), (long long)cs->case_end);
                else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_end);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  %s .L.case.%d\n", is_uns ? "b.ls" : "b.le", cs->label_id);
#else
                if (cs->case_val == (int32_t)cs->case_val)
                    printf("  cmp %s, %lld\n", reg(cond, sz), (long long)cs->case_val);
                else {
                    int tmp = alloc_reg();
                    printf("  movabs %s, %lld\n", reg64[tmp], (long long)cs->case_val);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  %s .L.skip.%d\n", is_uns ? "jb" : "jl", skip_lbl);
                if (cs->case_end == (int32_t)cs->case_end)
                    printf("  cmp %s, %lld\n", reg(cond, sz), (long long)cs->case_end);
                else {
                    int tmp = alloc_reg();
                    printf("  movabs %s, %lld\n", reg64[tmp], (long long)cs->case_end);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  %s .L.case.%d\n", is_uns ? "jbe" : "jle", cs->label_id);
#endif
                printf(".L.skip.%d:\n", skip_lbl);
            } else {
#ifdef ARCH_ARM64
                if ((cs->case_val >= 0 && cs->case_val <= 4095) ||
                    (cs->case_val > 0 && cs->case_val <= 0xffffff && (cs->case_val % 4096) == 0)) {
                    printf("  cmp %s, #%lld\n", reg(cond, sz), (long long)cs->case_val);
                } else {
                    int tmp = alloc_reg();
                    emit_mov_imm64(reg64[tmp], (uint64_t)cs->case_val);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  b.eq .L.case.%d\n", cs->label_id);
#else
                if (cs->case_val == (int32_t)cs->case_val) {
                    printf("  cmp %s, %lld\n", reg(cond, sz), (long long)cs->case_val);
                } else {
                    int tmp = alloc_reg();
                    printf("  movabs %s, %lld\n", reg64[tmp], (long long)cs->case_val);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  je .L.case.%d\n", cs->label_id);
#endif
            }
        }
        if (node->default_case) {
            if (!node->default_case->label_id)
                node->default_case->label_id = ++rcc_label_count;
#ifdef ARCH_ARM64
            printf("  b .L.case.%d\n", node->default_case->label_id);
        } else {
            printf("  b .L.end.%d\n", c);
#else
            printf("  jmp .L.case.%d\n", node->default_case->label_id);
        } else {
            printf("  jmp .L.end.%d\n", c);
#endif
        }
        free_reg(cond);
        break_stack[ctrl_depth] = c;
        continue_stack[ctrl_depth] = ctrl_depth > 0 ? continue_stack[ctrl_depth - 1] : c;
        ctrl_depth++;
        int r_body = gen(node->then);
        if (r_body != -1) free_reg(r_body);
        ctrl_depth--;
        printf(".L.end.%d:\n", c);
        return -1;
    }
    case ND_CASE: {
        if (!node->label_id)
            node->label_id = ++rcc_label_count;
        printf(".L.case.%d:\n", node->label_id);
        return gen(node->lhs);
    }
    case ND_BREAK:
        if (ctrl_depth == 0)
            error("stray break");
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
#ifdef ARCH_ARM64
        printf("  b .L.end.%d\n", break_stack[ctrl_depth - 1]);
#else
        printf("  jmp .L.end.%d\n", break_stack[ctrl_depth - 1]);
#endif
        return -1;
    case ND_CONTINUE:
        if (ctrl_depth == 0)
            error("stray continue");
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
#ifdef ARCH_ARM64
        printf("  b .L.continue.%d\n", continue_stack[ctrl_depth - 1]);
#else
        printf("  jmp .L.continue.%d\n", continue_stack[ctrl_depth - 1]);
#endif
        return -1;
    case ND_GOTO:
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        emit_vla_dealloc(node->cleanup_begin, node->cleanup_end);
#ifdef ARCH_ARM64
        printf("  b .L.label.%s.%s\n", current_fn, node->label_name);
#else
        printf("  jmp .L.label.%s.%s\n", current_fn, node->label_name);
#endif
        return -1;
    case ND_GOTO_IND: {
        int r = gen(node->lhs);
#ifdef ARCH_ARM64
        printf("  br %s\n", reg64[r]);
#else
        printf("  jmp %s\n", reg64[r]);
#endif
        free_reg(r);
        return -1;
    }
    case ND_LABEL: {
        printf(".L.label.%s.%s:\n", current_fn, node->label_name);
        return gen(node->lhs);
    }
    case ND_LABEL_VAL: {
        int r = alloc_reg();
#ifdef ARCH_ARM64
        emit_adrp_add(reg64[r], format(".L.label.%s.%s", current_fn, node->label_name));
#else
        printf("  lea %s, [rip + .L.label.%s.%s]\n", reg64[r], current_fn, node->label_name);
#endif
        return r;
    }
    case ND_ASM: {
#ifdef ARCH_ARM64
        error_tok(node->tok, "inline asm not implemented for ARM64");
        return -1;
#else
        // Evaluate operands: outputs (memory addr) then inputs (register value)
        int op_regs[MAX_ASM_OPERANDS];
        for (int i = 0; i < node->asm_noperands; i++) op_regs[i] = -1;

        for (int i = 0; i < node->asm_noperands; i++) {
            AsmOperand *op = &node->asm_ops[i];
            if (op->is_memory || (op->is_output && !op->is_rw)) {
                // memory operand or output: compute address
                int r = gen_addr(op->expr);
                op_regs[i] = r;
                op->reg = r;
                snprintf(op->asm_str, sizeof(op->asm_str), "(%%%s)", reg64[r]);
            } else {
                // register input: load value
                int r = gen(op->expr);
                op_regs[i] = r;
                op->reg = r;
                int sz = op->expr->ty ? op->expr->ty->size : 4;
                snprintf(op->asm_str, sizeof(op->asm_str), "%%%s", reg(r, sz));
            }
        }

        // Emit template with operand substitution, wrapped in AT&T syntax
        printf(".att_syntax prefix\n");

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
        printf("%s\n", out);

        printf(".intel_syntax noprefix\n");

        for (int i = 0; i < node->asm_noperands; i++)
            if (op_regs[i] >= 0) free_reg(op_regs[i]);
        return -1;
#endif
    }
    case ND_VA_START: {
        int r = gen(node->lhs);
        printf("  mov dword ptr [%s], %d\n", reg64[r], va_gp_start);
        printf("  mov dword ptr [%s + 4], %d\n", reg64[r], va_fp_start);
        printf("  lea rdx, [rbp+%d]\n", va_st_start);
        printf("  mov [%s + 8], rdx\n", reg64[r]);
        printf("  lea rdx, [rbp-%d]\n", va_reg_save_ofs);
        printf("  mov [%s + 16], rdx\n", reg64[r]);
        free_reg(r);
        return -1;
    }
    case ND_VA_COPY: {
        int rd = gen(node->lhs);
        printf("  push %s\n", reg64[rd]);
        free_reg(rd);
        int rs = gen(node->rhs);
        int rpop = alloc_reg();
        printf("  pop %s\n", reg64[rpop]);
        printf("  mov rcx, %s\n", reg64[rs]);
        printf("  mov [%s], rcx\n", reg64[rpop]);
        printf("  mov rcx, [%s + 8]\n", reg64[rs]);
        printf("  mov [%s + 8], rcx\n", reg64[rpop]);
        printf("  mov rcx, [%s + 16]\n", reg64[rs]);
        printf("  mov [%s + 16], rcx\n", reg64[rpop]);
        free_reg(rs);
        free_reg(rpop);
        return -1;
    }
    case ND_VA_ARG: {
        int r = gen(node->lhs);
        Type *ty = node->ty->base;
        bool is_fp = is_flonum(ty);
        bool is_ptr_struct = (ty->kind == TY_STRUCT || ty->kind == TY_UNION) && ty->size > 8;
        if (is_fp) {
            printf("  cmp dword ptr [%s + 4], 160\n", reg64[r]);
            printf("  ja .L.va_overflow.%d\n", rcc_label_count);
            printf("  mov ecx, dword ptr [%s + 4]\n", reg64[r]);
            printf("  add rcx, [%s + 16]\n", reg64[r]);
            printf("  add dword ptr [%s + 4], 16\n", reg64[r]);
            printf("  jmp .L.va_done.%d\n", rcc_label_count);
        } else if (is_ptr_struct) {
            printf("  cmp dword ptr [%s], 40\n", reg64[r]);
            printf("  ja .L.va_overflow.%d\n", rcc_label_count);
            printf("  mov ecx, dword ptr [%s]\n", reg64[r]);
            printf("  add rcx, [%s + 16]\n", reg64[r]);
            printf("  mov rcx, [rcx]\n");
            printf("  add dword ptr [%s], 8\n", reg64[r]);
            printf("  jmp .L.va_done.%d\n", rcc_label_count);
        } else {
            printf("  cmp dword ptr [%s], 40\n", reg64[r]);
            printf("  ja .L.va_overflow.%d\n", rcc_label_count);
            printf("  mov ecx, dword ptr [%s]\n", reg64[r]);
            printf("  add rcx, [%s + 16]\n", reg64[r]);
            printf("  add dword ptr [%s], 8\n", reg64[r]);
            printf("  jmp .L.va_done.%d\n", rcc_label_count);
        }
        printf(".L.va_overflow.%d:\n", rcc_label_count);
        printf("  mov rcx, [%s + 8]\n", reg64[r]);
        printf("  lea rdx, [rcx + 8]\n");
        printf("  mov [%s + 8], rdx\n", reg64[r]);
        if (is_ptr_struct)
            printf("  mov rcx, [rcx]\n");
        printf(".L.va_done.%d:\n", rcc_label_count);
        printf("  mov %s, rcx\n", reg64[r]);
        rcc_label_count++;
        return r;
    }
    default:
        break;
    }

    int r_lhs = gen(node->lhs);

    // Float binary operations (must come before integer ops)
    if (is_flonum(node->ty) && (node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_MUL || node->kind == ND_DIV)) {
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        printf("  fmov d0, %s\n", reg64[r_lhs]);
        printf("  fmov d1, %s\n", reg64[r_rhs]);
        char *inst = "";
        if (node->kind == ND_ADD) inst = "fadd";
        else if (node->kind == ND_SUB)
            inst = "fsub";
        else if (node->kind == ND_MUL)
            inst = "fmul";
        else if (node->kind == ND_DIV)
            inst = "fdiv";
        printf("  %s d0, d0, d1\n", inst);
        if (node->ty->kind == TY_FLOAT) {
            printf("  fcvt s0, d0\n");
            printf("  fcvt d0, s0\n");
        }
        printf("  fmov %s, d0\n", reg64[r_lhs]);
#else
        printf("  movq xmm0, %s\n", reg64[r_lhs]);
        printf("  movq xmm1, %s\n", reg64[r_rhs]);
        free_reg(r_rhs);
        char *inst = "";
        if (node->kind == ND_ADD) inst = "addsd";
        else if (node->kind == ND_SUB)
            inst = "subsd";
        else if (node->kind == ND_MUL)
            inst = "mulsd";
        else if (node->kind == ND_DIV)
            inst = "divsd";
        printf("  %s xmm0, xmm1\n", inst);
        if (node->ty->kind == TY_FLOAT) {
            printf("  cvtsd2ss xmm0, xmm0\n");
            printf("  cvtss2sd xmm0, xmm0\n");
        }
        printf("  movq %s, xmm0\n", reg64[r_lhs]);
#endif
        free_reg(r_rhs);
        return r_lhs;
    }

    // Float comparisons
    if ((node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) &&
        node->lhs->ty && node->rhs->ty && (is_flonum(node->lhs->ty) || is_flonum(node->rhs->ty))) {
        int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
        printf("  fmov d0, %s\n", reg64[r_lhs]);
        printf("  fmov d1, %s\n", reg64[r_rhs]);
        printf("  fcmp d0, d1\n");
        const char *cset_cond = "eq";
        if (node->kind == ND_EQ) cset_cond = "eq";
        else if (node->kind == ND_NE)
            cset_cond = "ne";
        else if (node->kind == ND_LT)
            cset_cond = "mi";
        else if (node->kind == ND_LE)
            cset_cond = "ls";
        printf("  cset %s, %s\n", reg(r_lhs, 4), cset_cond);
#else
        printf("  movq xmm0, %s\n", reg64[r_lhs]);
        printf("  movq xmm1, %s\n", reg64[r_rhs]);
        printf("  ucomisd xmm0, xmm1\n");
        if (node->kind == ND_EQ) {
            printf("  sete al\n");
            printf("  setnp cl\n");
            printf("  and al, cl\n");
        } else if (node->kind == ND_NE) {
            printf("  setne al\n");
            printf("  setp cl\n");
            printf("  or al, cl\n");
        } else if (node->kind == ND_LT) {
            printf("  setb al\n");
        } else if (node->kind == ND_LE) {
            printf("  setbe al\n");
        }
        printf("  movzx %s, al\n", reg(r_lhs, 4));
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
        if (node->kind == ND_DIV) {
            printf("  %s %s, %s, %s\n", is_unsigned ? "udiv" : "sdiv", reg(r_lhs, sz), reg(r_lhs, sz), reg(r_rhs, sz));
        } else {
            int tmp = alloc_reg();
            printf("  %s %s, %s, %s\n", is_unsigned ? "udiv" : "sdiv", reg(tmp, sz), reg(r_lhs, sz), reg(r_rhs, sz));
            printf("  mul %s, %s, %s\n", reg(tmp, sz), reg(tmp, sz), reg(r_rhs, sz));
            printf("  sub %s, %s, %s\n", reg(r_lhs, sz), reg(r_lhs, sz), reg(tmp, sz));
            free_reg(tmp);
        }
#else
        printf("  mov %s, %s\n", sz == 8 ? "rax" : "eax", reg(r_lhs, sz));
        if (is_unsigned) {
            if (sz == 8)
                printf("  xor edx, edx\n");
            else
                printf("  xor edx, edx\n");
        } else {
            if (sz == 8) printf("  cqo\n");
            else
                printf("  cdq\n");
        }
        printf("  %s %s\n", is_unsigned ? "div" : "idiv", reg(r_rhs, sz));
        printf("  mov %s, %s\n", reg(r_lhs, sz), node->kind == ND_DIV ? (sz == 8 ? "rax" : "eax") : (sz == 8 ? "rdx" : "edx"));
#endif
        free_reg(r_rhs);
        return r_lhs;
    }

    // Binary operators with potential immediate/memory optimization for RHS
    if (node->kind == ND_SHL || node->kind == ND_SHR) {
        int sz = op_size(node->ty);
#ifdef ARCH_ARM64
        const char *shift_op = node->kind == ND_SHL ? "lsl" : (use_unsigned(node->ty) ? "lsr" : "asr");
        if (node->rhs->kind == ND_NUM) {
            int s = (int)node->rhs->val;
            if (s >= sz * 8) {
                printf("  mov %s, #0\n", reg(r_lhs, sz));
            } else {
                printf("  %s %s, %s, #%d\n", shift_op, reg(r_lhs, sz), reg(r_lhs, sz), s);
            }
        } else {
            int r_rhs = gen(node->rhs);
            printf("  %s %s, %s, %s\n", shift_op, reg(r_lhs, sz), reg(r_lhs, sz), reg(r_rhs, sz));
            free_reg(r_rhs);
        }
#else
        if (node->rhs->kind == ND_NUM) {
            printf("  %s %s, %d\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar"), reg(r_lhs, sz), (int)node->rhs->val);
        } else {
            int r_rhs = gen(node->rhs);
            printf("  mov ecx, %s\n", reg(r_rhs, 4));
            printf("  %s %s, cl\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar"), reg(r_lhs, sz));
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
                printf("  lsl %s, %s, #%d\n", reg(r_lhs, sz), reg(r_lhs, sz), shift);
            } else if (node->kind == ND_MUL) {
                // ARM64 mul doesn't take immediates; load into a scratch register
                int tmp = alloc_reg();
                printf("  mov %s, #%d\n", reg(tmp, sz), imm);
                printf("  mul %s, %s, %s\n", reg(r_lhs, sz), reg(r_lhs, sz), reg(tmp, sz));
                free_reg(tmp);
            } else if (!strcmp(inst, "cmp")) {
                printf("  cmp %s, #%d\n", reg(r_lhs, sz), imm);
            } else if (node->kind != ND_BITAND && node->kind != ND_BITOR && node->kind != ND_BITXOR &&
                       imm >= 0 && imm <= 4095) {
                // add/sub accept simple immediate; bitwise ops need bitmask encoding
                printf("  %s %s, %s, #%d\n", inst, reg(r_lhs, sz), reg(r_lhs, sz), imm);
            } else {
                int tmp = alloc_reg();
                printf("  mov %s, #%d\n", reg(tmp, sz), imm);
                printf("  %s %s, %s, %s\n", inst, reg(r_lhs, sz), reg(r_lhs, sz), reg(tmp, sz));
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
                printf("  shl %s, %d\n", reg(r_lhs, sz), shift);
            } else {
                printf("  %s %s, %d\n", inst, reg(r_lhs, sz), imm);
#endif
            }
        } else {
            int r_rhs = gen(node->rhs);
#ifdef ARCH_ARM64
            // Sign-extend rhs to 64 bits when operation is 64-bit but rhs was
            // computed as 32-bit signed. ARM64: use sxtw.
            if (sz == 8 && op_size(node->rhs->ty) == 4 && !use_unsigned(node->rhs->ty))
                printf("  sxtw %s, %s\n", reg64[r_rhs], reg32[r_rhs]);
            if (!strcmp(inst, "cmp")) {
                printf("  cmp %s, %s\n", reg(r_lhs, sz), reg(r_rhs, sz));
            } else {
                printf("  %s %s, %s, %s\n", inst, reg(r_lhs, sz), reg(r_lhs, sz), reg(r_rhs, sz));
            }
#else
            // Sign-extend rhs to 64 bits when operation is 64-bit but rhs was
            // computed as 32-bit signed (e.g. pointer + int, long + int).
            // 32-bit writes zero-extend in x86-64, so without this a negative
            // int would be treated as a large positive offset.
            if (sz == 8 && op_size(node->rhs->ty) == 4 && !use_unsigned(node->rhs->ty))
                printf("  movsxd %s, %s\n", reg64[r_rhs], reg(r_rhs, 4));
            printf("  %s %s, %s\n", inst, reg(r_lhs, sz), reg(r_rhs, sz));
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
                        printf("  sar %s, %d\n", reg(r_lhs, sz), shift);
                    } else {
                        // Non-power of 2: use idiv
                        printf("  mov %s, %s\n", sz == 8 ? "rax" : "eax", reg(r_lhs, sz));
                        if (sz == 8)
                            printf("  cqo\n");
                        else
                            printf("  cdq\n");
                        printf("  mov %s, %d\n", sz == 8 ? "r11" : "r11d", elem_sz);
                        printf("  idiv %s\n", sz == 8 ? "r11" : "r11d");
                        printf("  mov %s, %s\n", reg(r_lhs, sz), sz == 8 ? "rax" : "eax");
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
            printf("  cset %s, %s\n", reg(r_lhs, op_size(node->ty)), cset_cond);
#else
            char *set = "";
            if (node->kind == ND_EQ) set = "sete";
            else if (node->kind == ND_NE)
                set = "setne";
            else if (node->kind == ND_LT)
                set = use_unsigned_cmp(node) ? "setb" : "setl";
            else if (node->kind == ND_LE)
                set = use_unsigned_cmp(node) ? "setbe" : "setle";
            printf("  %s al\n", set);
            printf("  movzx %s, al\n", reg(r_lhs, op_size(node->ty)));
#endif
        }
        return r_lhs;
    }

    error("invalid expression %d", node->kind);
    return -1;
}

// Helper: map 64-bit register name to 32-bit
static const char *to32(const char *r64) {
    if (!strcmp(r64, "rax")) return "eax";
    if (!strcmp(r64, "rcx")) return "ecx";
    if (!strcmp(r64, "rdx")) return "edx";
    if (!strcmp(r64, "r8")) return "r8d";
    if (!strcmp(r64, "r9")) return "r9d";
    if (!strcmp(r64, "r10")) return "r10d";
    if (!strcmp(r64, "r11")) return "r11d";
    if (!strcmp(r64, "rbx")) return "ebx";
    if (!strcmp(r64, "r12")) return "r12d";
    if (!strcmp(r64, "r13")) return "r13d";
    if (!strcmp(r64, "r14")) return "r14d";
    if (!strcmp(r64, "r15")) return "r15d";
    if (!strcmp(r64, "rsi")) return "esi";
    return NULL;
}

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
static int peep_mov_rbp_reg(char *line, int *off, char *reg, int reg_sz) {
    if (strncmp(line, "  mov [rbp-", 11) != 0) return 0;
    char *p = line + 11;
    char *end = strchr(p, ']');
    if (!end) return 0;
    *end = '\0';
    *off = atoi(p);
    *end = ']';
    p = end + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    int len = strlen(p);
    if (len >= reg_sz) return 0;
    memcpy(reg, p, len + 1);
    return 1;
}
static int peep_mov_reg_rbp(char *line, char *dst, int dst_sz, char *szword, int *off) {
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
    if (strncmp(p, "dword ptr [rbp-", 15) == 0) {
        if (szword) strcpy(szword, "dword");
        p += 15;
    } else if (strncmp(p, "qword ptr [rbp-", 15) == 0) {
        if (szword) strcpy(szword, "qword");
        p += 15;
    } else
        return 0;
    char *end = strchr(p, ']');
    if (!end) return 0;
    *end = '\0';
    *off = atoi(p);
    *end = ']';
    return 1;
}
static int peep_mov_rbp_imm(char *line, int *off, int *val) {
    if (strncmp(line, "  mov dword ptr [rbp-", 21) != 0) return 0;
    char *p = line + 21;
    char *end = strchr(p, ']');
    if (!end) return 0;
    *end = '\0';
    *off = atoi(p);
    *end = ']';
    p = end + 1;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    *val = atoi(p);
    return 1;
}
static int peep_jmp(char *line, char *lbl, int lbl_sz) {
    if (strncmp(line, "  jmp ", 6) != 0) return 0;
    char *p = line + 6;
    int len = strlen(p);
    if (len >= lbl_sz) return 0;
    memcpy(lbl, p, len + 1);
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
static int peep_mov_reg_imm(char *line, char *reg, int reg_sz, int *imm) {
    if (strncmp(line, "  mov ", 6) != 0) return 0;
    char *p = line + 6;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int rlen = comma - p;
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen);
    reg[rlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    char *endp;
    long v = strtol(p, &endp, 0);
    if (endp == p) return 0;
    *imm = (int)v;
    return 1;
}
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
static int peep_mov_reg_mem(char *line, char *reg, int reg_sz, char *mem, int mem_sz) {
    if (strncmp(line, "  mov ", 6) != 0) return 0;
    char *p = line + 6;
    char *comma = strchr(p, ',');
    if (!comma) return 0;
    int rlen = comma - p;
    if (rlen >= reg_sz) return 0;
    memcpy(reg, p, rlen);
    reg[rlen] = '\0';
    p = comma + 1;
    while (*p == ' ' || *p == '\t') p++;
    int mlen = strlen(p);
    if (mlen >= mem_sz) return 0;
    memcpy(mem, p, mlen + 1);
    return 1;
}

// Helper: check if string is a register name
static int is_reg(const char *s) {
    return (s[0] == 'r' || s[0] == 'e' || !strncmp(s, "si", 2) || !strncmp(s, "bl", 2) || !strncmp(s, "bx", 2));
}

// Check if a physical register is used after line 'after' before being overwritten
static int reg_live_after(char **lines, int nlines, int after, int pid) {
    const char *variants[6] = {0};
    int nv = 0;
    for (int vi = 0; vi < 8; vi++) {
        if (phys_reg_id(reg64[vi]) == pid) {
            variants[nv++] = reg64[vi];
            variants[nv++] = reg32[vi];
            break;
        }
    }
    if (pid == 0) {
        variants[nv++] = "rax";
        variants[nv++] = "eax";
        variants[nv++] = "al";
    } else if (pid == 1) {
        variants[nv++] = "rcx";
        variants[nv++] = "ecx";
        variants[nv++] = "cl";
    } else if (pid == 2) {
        variants[nv++] = "rdx";
        variants[nv++] = "edx";
    } else if (pid == 3) {
        variants[nv++] = "rbx";
        variants[nv++] = "ebx";
        variants[nv++] = "bl";
    }
    for (int k = after + 1; k < nlines && k < after + 30; k++) {
        if (!lines[k] || !lines[k][0]) continue;
        if (lines[k][0] != ' ') return 1;
        for (int vi = 0; vi < nv && variants[vi]; vi++) {
            if (strstr(lines[k], variants[vi])) return 1;
        }
    }
    return 0;
}

void codegen(Program *prog) {
    cg_stream = stdout;
    all_items = prog->items;
    all_strs = prog->strs;
    alloca_needed = false;
    // Assembly header
#ifndef ARCH_ARM64
    printf(".intel_syntax noprefix\n");
#endif
#ifndef _WIN32
#if !defined(__APPLE__)
    printf(".section .note.GNU-stack,\"\",@progbits\n");
#endif
#endif

    // Emit data section for strings
    if (prog->globals || prog->strs || float_lits) {
        printf("\n.data\n");
        for (LVar *var = prog->globals; var; var = var->next) {
            if (var->is_extern)
                continue;
            // Skip function symbols - they're emitted as code in the text section
            // (or not at all for inline functions without body)
            if (var->is_function)
                continue;
            char *label = var->asm_name ? var->asm_name : var->name;
            bool reserved = !var->asm_name && is_asm_reserved(var->name);
            char *safe_label = reserved ? format(".L_rcc_%s", var->name) : label;
            if (var->ty->align > 1)
                printf("  .balign %d\n", var->ty->align);
            if (!var->is_static)
                printf(".globl %s\n", sym_name(label));
            printf("%s:\n", safe_label);
            if (reserved)
                printf(".set %s, %s\n", label, safe_label);
            if (var->init_data || var->relocs) {
                int pos = 0;
                for (Reloc *rel = var->relocs; rel; rel = rel->next) {
                    for (; pos < rel->offset; pos++)
                        printf("  .byte %u\n", (unsigned char)var->init_data[pos]);
                    if (rel->addend)
                        printf("  .quad %s%+d\n", rel->label, rel->addend);
                    else
                        printf("  .quad %s\n", rel->label);
                    pos += 8;
                }
                for (; pos < var->init_size; pos++)
                    printf("  .byte %u\n", (unsigned char)var->init_data[pos]);
                if (var->ty->size > var->init_size)
                    printf("  .zero %d\n", var->ty->size - var->init_size);
            } else if (var->has_init) {
                if (var->ty->size == 1)
                    printf("  .byte %u\n", (unsigned char)var->init_val);
                else if (var->ty->size == 2)
                    printf("  .word %u\n", (unsigned short)var->init_val);
                else if (var->ty->size == 4)
                    printf("  .long %u\n", (unsigned)var->init_val);
                else
                    printf("  .quad %lld\n", (long long)var->init_val);
            } else if (var->ty->size > 0) {
                printf("  .zero %d\n", var->ty->size);
            }
        }
        for (StrLit *s = prog->strs; s; s = s->next) {
            printf(".LC%d:\n", s->id);
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
                    if (s->elem_size == 2)
                        printf("  .2byte %u\n", c);
                    else
                        printf("  .4byte %u\n", c);
                }
                if (s->elem_size == 2)
                    printf("  .2byte 0\n");
                else
                    printf("  .4byte 0\n");
            } else {
                for (int i = 0; i < s->len; i++) {
                    printf("  .byte %u\n", (unsigned char)s->str[i]);
                }
                printf("  .byte 0\n"); // null terminator
            }
        }
        printf("\n.text\n");
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
        fn_struct_ret_off = 0;
        fn_struct_ret_total = 0;

        // Pass 1: Generate function body to temp file to discover register usage
        FILE *body_file = tmpfile();
        cg_stream = body_file;
        used_regs = 0;
        ever_used_regs = 0;
        spilled_regs = 0;
        spill_count = 0;
        ctrl_depth = 0;
        fn_uses_alloca = false;

        // Save params to locals (emitted to body buffer, will be after prologue)
        // ARM64: handled in the Pass 2 prologue instead
        int param_xmm_index = 0;
        int stack_param_index = 0;
        int param_index = fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION) ? 1 : 0;
#ifndef ARCH_ARM64
#ifdef _WIN32
        char *param_regs64[] = {"rcx", "rdx", "r8", "r9"};
        char *param_regs32[] = {"ecx", "edx", "r8d", "r9d"};
        char *param_regs16[] = {"cx", "dx", "r8w", "r9w"};
        char *param_regs8[] = {"cl", "dl", "r8b", "r9b"};
        char *param_xmm[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
        int max_param_regs = 4;
#else
        char *param_regs64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        char *param_regs32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
        char *param_regs16[] = {"di", "si", "dx", "cx", "r8w", "r9w"};
        char *param_regs8[] = {"dil", "sil", "dl", "cl", "r8b", "r9b"};
        char *param_xmm[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
        int max_param_regs = 6;
        int max_param_xmm = 8;
#endif
        for (LVar *var = fn->params; var; var = var->param_next) {
#ifdef _WIN32
            if (is_flonum(var->ty)) {
                if (param_index < max_param_regs) {
                    if (var->ty->size == 4) {
                        printf("  cvtsd2ss xmm0, %s\n", param_xmm[param_index]);
                        printf("  movss dword ptr [rbp-%d], xmm0\n", var->offset);
                    } else {
                        printf("  movsd qword ptr [rbp-%d], %s\n", var->offset, param_xmm[param_index]);
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
                    printf("  mov r11, %s\n", preg);
                    printf("  mov r10, %d\n", var->ty->size);
                    printf(".L.pcopy.%d:\n", c);
                    printf("  cmp r10, 0\n");
                    printf("  je .L.pcopy_end.%d\n", c);
                    printf("  mov al, byte ptr [r11 + r10 - 1]\n");
                    printf("  mov byte ptr [rbp-%d + r10 - 1], al\n", var->offset);
                    printf("  sub r10, 1\n");
                    printf("  jmp .L.pcopy.%d\n", c);
                    printf(".L.pcopy_end.%d:\n", c);
                } else {
                    printf("  mov [rbp-%d], %s\n", var->offset, preg);
                }
                param_index++;
            } else {
                // Stack argument on Windows (shadow space = 32 bytes)
                int stack_off = 48 + stack_param_index * 8;
                if (is_flonum(var->ty)) {
                    if (var->ty->size == 4) {
                        printf("  movss xmm0, dword ptr [rbp+%d]\n", stack_off);
                        printf("  movss dword ptr [rbp-%d], xmm0\n", var->offset);
                    } else {
                        printf("  movsd xmm0, qword ptr [rbp+%d]\n", stack_off);
                        printf("  movsd qword ptr [rbp-%d], xmm0\n", var->offset);
                    }
                } else {
                    char *tmpreg = var->ty->size == 1 ? "al"
                        : var->ty->size == 2          ? "ax"
                        : var->ty->size <= 4          ? "eax"
                                                      : "rax";
                    printf("  mov %s, [rbp+%d]\n", tmpreg, stack_off);
                    printf("  mov [rbp-%d], %s\n", var->offset, tmpreg);
                }
                stack_param_index++;
            }
#else
            if (is_flonum(var->ty)) {
                if (param_xmm_index < max_param_xmm) {
                    if (var->ty->size == 4) {
                        printf("  cvtsd2ss xmm0, %s\n", param_xmm[param_xmm_index]);
                        printf("  movss dword ptr [rbp-%d], xmm0\n", var->offset);
                    } else {
                        printf("  movsd qword ptr [rbp-%d], %s\n", var->offset, param_xmm[param_xmm_index]);
                    }
                    param_xmm_index++;
                } else {
                    if (var->ty->size == 4) {
                        printf("  movss xmm0, dword ptr [rbp+%d]\n", 16 + stack_param_index * 8);
                        printf("  movss dword ptr [rbp-%d], xmm0\n", var->offset);
                    } else {
                        printf("  movsd xmm0, qword ptr [rbp+%d]\n", 16 + stack_param_index * 8);
                        printf("  movsd qword ptr [rbp-%d], xmm0\n", var->offset);
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
                    printf("  mov r11, %s\n", preg);
                    printf("  mov r10, %d\n", var->ty->size);
                    printf(".L.pcopy.%d:\n", c);
                    printf("  cmp r10, 0\n");
                    printf("  je .L.pcopy_end.%d\n", c);
                    printf("  mov al, byte ptr [r11 + r10 - 1]\n");
                    printf("  mov byte ptr [rbp-%d + r10 - 1], al\n", var->offset);
                    printf("  sub r10, 1\n");
                    printf("  jmp .L.pcopy.%d\n", c);
                    printf(".L.pcopy_end.%d:\n", c);
                } else {
                    printf("  mov [rbp-%d], %s\n", var->offset, preg);
                }
                param_index++;
            } else {
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                    int c = ++rcc_label_count;
                    printf("  mov r10, %d\n", var->ty->size);
                    printf(".L.pcopy.%d:\n", c);
                    printf("  cmp r10, 0\n");
                    printf("  je .L.pcopy_end.%d\n", c);
                    printf("  mov al, byte ptr [rbp+%d + r10 - 1]\n", 16 + stack_param_index * 8);
                    printf("  mov byte ptr [rbp-%d + r10 - 1], al\n", var->offset);
                    printf("  sub r10, 1\n");
                    printf("  jmp .L.pcopy.%d\n", c);
                    printf(".L.pcopy_end.%d:\n", c);
                } else {
                    char *tmpreg = var->ty->size == 1 ? "al"
                        : var->ty->size == 2          ? "ax"
                        : var->ty->size <= 4          ? "eax"
                                                      : "rax";
                    printf("  mov %s, [rbp+%d]\n", tmpreg, 16 + stack_param_index * 8);
                    printf("  mov [rbp-%d], %s\n", var->offset, tmpreg);
                }
                stack_param_index++;
            }
#endif
        }
#endif /* !ARCH_ARM64 */

        // Save arg registers if function is variadic
        if (fn->is_variadic) {
            int gp_count = param_index;
            int va_fp = param_xmm_index;
            va_reg_save_ofs = current_fn_stack_size + 176;
            va_gp_start = gp_count * 8;
            va_fp_start = va_fp * 16 + 48;
            va_st_start = 16 + stack_param_index * 8;

            switch (gp_count) {
            case 0: printf("  mov [rbp-%d], rdi\n", va_reg_save_ofs); /* fallthrough */
            case 1: printf("  mov [rbp-%d], rsi\n", va_reg_save_ofs - 8); /* fallthrough */
            case 2: printf("  mov [rbp-%d], rdx\n", va_reg_save_ofs - 16); /* fallthrough */
            case 3: printf("  mov [rbp-%d], rcx\n", va_reg_save_ofs - 24); /* fallthrough */
            case 4: printf("  mov [rbp-%d], r8\n", va_reg_save_ofs - 32); /* fallthrough */
            case 5: printf("  mov [rbp-%d], r9\n", va_reg_save_ofs - 40);
            }
            if (va_fp < 8) {
                printf("  test al, al\n");
                printf("  je .L.va_no_xmm.%d\n", rcc_label_count);
                switch (va_fp) {
                case 0: printf("  movaps [rbp-%d], xmm0\n", va_reg_save_ofs - 48); /* fallthrough */
                case 1: printf("  movaps [rbp-%d], xmm1\n", va_reg_save_ofs - 64); /* fallthrough */
                case 2: printf("  movaps [rbp-%d], xmm2\n", va_reg_save_ofs - 80); /* fallthrough */
                case 3: printf("  movaps [rbp-%d], xmm3\n", va_reg_save_ofs - 96); /* fallthrough */
                case 4: printf("  movaps [rbp-%d], xmm4\n", va_reg_save_ofs - 112); /* fallthrough */
                case 5: printf("  movaps [rbp-%d], xmm5\n", va_reg_save_ofs - 128); /* fallthrough */
                case 6: printf("  movaps [rbp-%d], xmm6\n", va_reg_save_ofs - 144); /* fallthrough */
                case 7: printf("  movaps [rbp-%d], xmm7\n", va_reg_save_ofs - 160);
                }
                printf(".L.va_no_xmm.%d:\n", rcc_label_count);
                rcc_label_count++;
            }
        }

        for (Node *n = fn->body; n; n = n->next) {
            int r = gen(n);
            if (r != -1) free_reg(r);
        }
        fflush(body_file);

        // Pass 2: Emit optimized prologue, body, epilogue to stdout
        cg_stream = stdout;

#ifdef ARCH_ARM64
        // === ARM64 prologue ===
        // Callee-saved: x19-x28 are callee-saved among our 12 allocatable regs
        // x19-x24 = indices 6-11 in our reg arrays
        int callee_mask = ((ever_used_regs >> 6) & 63);
        int n_callee_saved = 0;
        for (int j = 0; j < 6; j++)
            if (callee_mask & (1 << j)) n_callee_saved++;

        int need = fn->stack_size + fn_struct_ret_total + 32;
        int frame_size = need + 16 + n_callee_saved * 8;
        // Round up to 16-byte alignment
        frame_size = (frame_size + 15) & ~15;

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
        if (fn->is_weak)
            printf(".weak %s\n", sym_name(fn->name));
        else if (fn_exported)
            printf(".globl %s\n", sym_name(fn->name));
        if (fn_label != fn->name)
            printf("%s = %s\n", fn->name, fn_label);
        else if (fn->asm_name && (fn_exported || fn->is_weak))
            printf("%s = %s\n", fn->name, fn->asm_name);
        printf("%s:\n", fn_exported ? sym_name(fn_label) : fn_label);

        // Stack frame: stp fp,lr; mov fp,sp; sub sp,sp,#frame_size
        printf("  stp %s, %s, [%s, #-16]!\n", FRAME_PTR, LINK_REG, STACK_REG);
        printf("  mov %s, %s\n", FRAME_PTR, STACK_REG);
        if (frame_size <= 4095)
            printf("  sub %s, %s, #%d\n", STACK_REG, STACK_REG, frame_size);
        else {
            int fs = frame_size;
            printf("  mov x16, #%d\n", fs & 0xffff);
            fs >>= 16;
            int s = 16;
            while (fs) {
                printf("  movk x16, #%d, lsl #%d\n", fs & 0xffff, s);
                fs >>= 16;
                s += 16;
            }
            printf("  sub %s, %s, x16\n", STACK_REG, STACK_REG);
        }

        // Save callee-saved regs
        int cs_off = 16;
        for (int j = 0; j < 6; j++) {
            if (callee_mask & (1 << j)) {
                printf("  str %s, [%s, #%d]\n", reg64[j + 6], STACK_REG, cs_off);
                cs_off += 8;
            }
        }

        // Save hidden struct return pointer if needed
        if (fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION)) {
            int retbuf_offset = 0;
            for (LVar *var = fn->locals; var; var = var->next) {
                if (var->name && var->name[0] == '\0') {
                    retbuf_offset = var->offset;
                    break;
                }
            }
            if (retbuf_offset <= 4095)
                printf("  str x8, [%s, #-%d]\n", FRAME_PTR, retbuf_offset);
            else {
                int v = retbuf_offset;
                printf("  mov x16, #%d\n", v & 0xffff);
                v >>= 16;
                int s = 16;
                while (v) {
                    printf("  movk x16, #%d, lsl #%d\n", v & 0xffff, s);
                    v >>= 16;
                    s += 16;
                }
                printf("  sub x16, %s, x16\n", FRAME_PTR);
                printf("  str x8, [x16]\n");
            }
        }

        // Save incoming params to stack slots
        {
            int gp_param = 0;
            int fp_param = 0;
            int stack_param = 0;
            char *gpreg[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
            char *wpreg[] = {"w0", "w1", "w2", "w3", "w4", "w5", "w6", "w7"};
            char *fpreg[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7"};
            for (LVar *var = fn->params; var; var = var->param_next) {
                if (is_flonum(var->ty)) {
                    if (fp_param < 8) {
                        printf("  str %s, [%s, #-%d]\n", fpreg[fp_param], FRAME_PTR, var->offset);
                        fp_param++;
                    }
                } else if (gp_param < 8) {
                    char *preg = var->ty->size <= 4 ? wpreg[gp_param] : gpreg[gp_param];
                    if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                        int c = ++rcc_label_count;
                        printf("  mov x11, %s\n", preg);
                        if (var->offset <= 4095)
                            printf("  sub x13, %s, #%d\n", FRAME_PTR, var->offset);
                        else {
                            int v = var->offset;
                            printf("  mov x13, #%d\n", v & 0xffff);
                            v >>= 16;
                            int s = 16;
                            while (v) {
                                printf("  movk x13, #%d, lsl #%d\n", v & 0xffff, s);
                                v >>= 16;
                                s += 16;
                            }
                            printf("  sub x13, %s, x13\n", FRAME_PTR);
                        }
                        printf("  mov x12, #%d\n", var->ty->size);
                        printf(".L.pcopy.%d:\n", c);
                        printf("  cmp x12, #0\n");
                        printf("  b.eq .L.pcopy_end.%d\n", c);
                        printf("  sub x12, x12, #1\n");
                        printf("  ldrb w14, [x11, x12]\n");
                        printf("  strb w14, [x13, x12]\n");
                        printf("  b .L.pcopy.%d\n", c);
                        printf(".L.pcopy_end.%d:\n", c);
                    } else {
                        printf("  str %s, [%s, #-%d]\n", preg, FRAME_PTR, var->offset);
                    }
                    gp_param++;
                } else {
                    // Stack argument — load from caller's frame
                    printf("  ldr x11, [%s, #%d]\n", FRAME_PTR, 16 + stack_param * 8);
                    printf("  str x11, [%s, #-%d]\n", FRAME_PTR, var->offset);
                    stack_param++;
                }
            }
        }

        // Read body into lines
        fseek(body_file, 0, SEEK_END);
        long body_len = ftell(body_file);
        fseek(body_file, 0, SEEK_SET);
        char *body_text = malloc(body_len + 1);
        fread(body_text, 1, body_len, body_file);
        body_text[body_len] = '\0';
        fclose(body_file);

        // Emit body (skip peephole for now)
        fputs(body_text, stdout);

        // === ARM64 epilogue ===
        printf(".L.return.%s:\n", fn->name);

        // Cleanup calls
        bool has_cleanup = false;
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var)) {
                has_cleanup = true;
                break;
            }
        if (has_cleanup)
            printf("  str x0, [%s, #-8]\n", FRAME_PTR); // save return value
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var))
                emit_cleanup_var(var);
        if (has_cleanup)
            printf("  ldr x0, [%s, #-8]\n", FRAME_PTR);

        // Restore callee-saved
        cs_off = 16;
        for (int j = 0; j < 6; j++) {
            if (callee_mask & (1 << j)) {
                printf("  ldr %s, [%s, #%d]\n", reg64[j + 6], STACK_REG, cs_off);
                cs_off += 8;
            }
        }

        if (frame_size <= 4095)
            printf("  add %s, %s, #%d\n", STACK_REG, STACK_REG, frame_size);
        else {
            int fs = frame_size;
            printf("  mov x16, #%d\n", fs & 0xffff);
            fs >>= 16;
            int s = 16;
            while (fs) {
                printf("  movk x16, #%d, lsl #%d\n", fs & 0xffff, s);
                fs >>= 16;
                s += 16;
            }
            printf("  add %s, %s, x16\n", STACK_REG, STACK_REG);
        }
        printf("  ldp %s, %s, [%s], #16\n", FRAME_PTR, LINK_REG, STACK_REG);
        printf("  ret\n");

        free(body_text);
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
        // Reserve space for register spill slots (deepest at rbp-120)
        if (spill_count > 0 && need < 120)
            need = 120;
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
            printf(".weak %s\n", sym_name(fn->name));
        } else if (fn_exported) {
            printf(".globl %s\n", sym_name(fn->name));
        }
        // When the function name collides with a reserved name (e.g. register),
        // emit an alias from the real name to the safe label so that references
        // resolve correctly.
        if (fn_label != fn->name)
            printf("%s = %s\n", fn->name, fn_label);
        else if (fn->asm_name && (fn_exported || fn->is_weak))
            printf("%s = %s\n", fn->name, fn->asm_name);
        printf("%s:\n", fn_exported ? sym_name(fn_label) : fn_label);
        printf("  push rbp\n");
        printf("  mov rbp, rsp\n");

        // Only push callee-saved registers that were actually used
        for (int j = 0; j < callee_count; j++) {
            if (callee_mask & (1 << j))
                printf("  push %s\n", reg64[j + 2]);
        }
        printf("  sub rsp, %d\n", sub_amount);

        if (fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION)) {
            int retbuf_offset = 0;
            for (LVar *var = fn->locals; var; var = var->next) {
                if (var->name && var->name[0] == '\0') {
                    retbuf_offset = var->offset;
                    break;
                }
            }
            char *retreg =
#ifdef _WIN32
                "rcx"
#else
                "rdi"
#endif
                ;
            printf("  mov [rbp-%d], %s\n", retbuf_offset, retreg);
        }

        // Read body into lines, optimize, emit
        fseek(body_file, 0, SEEK_END);
        long body_len = ftell(body_file);
        fseek(body_file, 0, SEEK_SET);

        // Read all body text
        char *body_text = malloc(body_len + 1);
        size_t nread = fread(body_text, 1, body_len, body_file);
        (void)nread;
        body_text[body_len] = '\0';
        fclose(body_file);

// Process body in chunks to handle functions larger than PEEP_MAX lines.
#define PEEP_MAX 65536
        char *lines[PEEP_MAX];
        char *chunk = body_text;
        while (*chunk) {
            int nlines = 0;
            char *p = chunk;
            while (*p && nlines < PEEP_MAX) {
                lines[nlines] = p;
                char *nl = strchr(p, '\n');
                if (nl) {
                    *nl = '\0';
                    p = nl + 1;
                } else
                    p += strlen(p);
                nlines++;
            }
            chunk = p; // next chunk starts here

            // Run peephole on this chunk
            if (!opt_O0 && nlines < 50000) {
                for (int pass = 0; pass < 4; pass++) {
                    for (int li = 0; li < nlines - 1; li++) {
                        if (!lines[li] || !lines[li][0]) continue;
                        int lj = li + 1;
                        while (lj < nlines && (!lines[lj] || !lines[lj][0])) lj++;
                        if (lj >= nlines) break;

                        char d1[80], s1[80], d2[80], s2[80];

                        // Pattern 1: mov REG, SRC; mov DEST, REG -> mov DEST, SRC
                        if (peep_mov_reg_reg(lines[li], d1, sizeof(d1), s1, sizeof(s1)) &&
                            peep_mov_reg_reg(lines[lj], d2, sizeof(d2), s2, sizeof(s2)) &&
                            strcmp(s2, d1) == 0 && is_reg(d1) && is_reg(s1)) {
                            char newline[200];
                            snprintf(newline, sizeof(newline), "  mov %s, %s", d2, s1);
                            lines[lj] = strdup(newline);
                            int pid = phys_reg_id(d1);
                            if (pid >= 0 && !reg_live_after(lines, nlines, lj, pid))
                                lines[li] = "";
                            continue;
                        }

                        // ... rest of peephole patterns ...
                    }
                }
            }

            // Emit chunk
            for (int li = 0; li < nlines; li++) {
                if (lines[li] && lines[li][0])
                    fprintf(stdout, "%s\n", lines[li]);
            }
        }
        free(body_text);

        // Emit epilogue
        printf(".L.return.%s:\n", fn->name);

        // Emit __cleanup__ calls (LIFO: locals list is in reverse declaration order)
        bool has_cleanup = false;
        for (LVar *var = fn->locals; var; var = var->next)
            if (var_has_cleanup(var)) {
                has_cleanup = true;
                break;
            }
        if (has_cleanup)
            printf("  mov [rbp-%d], rax\n", SPILL_R10);
        for (LVar *var = fn->locals; var; var = var->next) {
            if (var_has_cleanup(var))
                emit_cleanup_var(var);
        }
        if (has_cleanup)
            printf("  mov rax, [rbp-%d]\n", SPILL_R10);
        if (fn->dealloc_vla)
            printf("  lea rsp, [rbp-%d]\n", n_pushes * 8);
        else if (fn_uses_alloca)
            printf("  lea rsp, [rbp-%d]\n", n_pushes * 8);
        else
            printf("  add rsp, %d\n", sub_amount);
        for (int j = callee_count - 1; j >= 0; j--) {
            if (callee_mask & (1 << j))
                printf("  pop %s\n", reg64[j + 2]);
        }
        printf("  pop rbp\n");
        printf("  ret\n");
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
#ifdef _WIN32
        printf("\n.section .ctors,\"w\"\n");
#elif defined(__APPLE__)
        printf("\n.section __DATA,__mod_init_func\n");
#else
                printf("\n.section .init_array,\"aw\",@init_array\n");
#endif
        for (TLItem *item = prog->items; item; item = item->next) {
            if (item->kind == TL_FUNC && item->fn->is_constructor)
                printf("  .quad %s\n", item->fn->name);
        }
    }
    if (has_dtor) {
#ifdef _WIN32
        printf("\n.section .dtors,\"w\"\n");
#elif defined(__APPLE__)
        printf("\n.section __DATA,__mod_term_func\n");
#else
                printf("\n.section .fini_array,\"aw\",@fini_array\n");
#endif
        for (TLItem *item = prog->items; item; item = item->next) {
            if (item->kind == TL_FUNC && item->fn->is_destructor)
                printf("  .quad %s\n", item->fn->name);
        }
    }

    if (alloca_needed)
        emit_alloca();

    // Emit float literal constants after all functions
    if (float_lits) {
#ifdef _WIN32
        printf("\n.section .rdata,\"dr\"\n");
#else
        printf("\n.section .rodata\n");
#endif
        printf("  .balign 8\n");
        for (FloatLit *fl = float_lits; fl; fl = fl->next) {
            printf(".LF%d:\n", fl->id);
            if (fl->size == 4) {
                float f = (float)fl->val;
                unsigned int bits;
                memcpy(&bits, &f, 4);
                printf("  .long %u\n", bits);
                printf("  .long 0\n");
            } else {
                unsigned long long bits;
                memcpy(&bits, &fl->val, 8);
                printf("  .quad %llu\n", bits);
            }
        }
    }
}
