#include "rcc.h"
#include <stdarg.h>

static FILE *cg_stream;
static Function *current_fn_def;
static Function *all_funcs;
static void cg_emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(cg_stream, fmt, ap);
    va_end(ap);
}
#define printf(...) cg_emit(__VA_ARGS__)

static char *current_fn;
static int rcc_label_count = 0;
static int break_stack[128];
static int continue_stack[128];
static int ctrl_depth = 0;
static int float_lit_count = 0;

typedef struct FloatLit FloatLit;
struct FloatLit {
    FloatLit *next;
    int id;
    double val;
    int size; // 4=float, 8=double
};
static FloatLit *float_lits;

static char *reg64[] = {"r10", "r11", "rbx", "r12", "r13", "r14", "r15", "rsi"};
static char *reg32[] = {"r10d", "r11d", "ebx", "r12d", "r13d", "r14d", "r15d", "esi"};
static char *reg16[] = {"r10w", "r11w", "bx", "r12w", "r13w", "r14w", "r15w", "si"};
static char *reg8[] = {"r10b", "r11b", "bl", "r12b", "r13b", "r14b", "r15b", "sil"};
static int used_regs = 0;
static int ever_used_regs = 0;

// Spill slot offsets from rbp for scratch register saves across calls
#define SPILL_R10 56
#define SPILL_R11 64

static char *reg(int r, int size);
static int alloc_reg(void);
static void free_reg(int i);
static int gen(Node *node);
static int gen_addr(Node *node);

static char *func_asm_name(char *name) {
    for (Function *fn = all_funcs; fn; fn = fn->next) {
        if (fn->name == name)
            return fn->asm_name ? fn->asm_name : fn->name;
    }
    return name;
}

static void emit_direct_call(char *name) {
    printf("  lea r11, [rip + %s]\n", func_asm_name(name));
    printf("  call r11\n");
}

static bool var_has_cleanup(LVar *var) {
    if (!var->is_local) return false;
    if (var->cleanup_func) return true;
    return var->ty->kind == TY_ARRAY && var->ty->base && var->ty->base->cleanup_func;
}

static void emit_cleanup_var(LVar *var) {
    if (var->cleanup_func) {
#ifdef _WIN32
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
#ifdef _WIN32
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
    if (!call_target && node->lhs && node->lhs->kind == ND_LVAR &&
        node->lhs->var && node->lhs->var->is_function)
        call_target = node->lhs->var->name;

#ifdef _WIN32
    char *argreg32[] = {"ecx", "edx", "r8d", "r9d"};
    char *argreg64[] = {"rcx", "rdx", "r8", "r9"};
    char *argxmm[] = {"xmm0", "xmm1", "xmm2", "xmm3"};
    int max_reg_args = 4;
    int shadow_space = 32;
#else
    char *argreg32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
    char *argreg64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
    char *argxmm[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
    int max_gp_args = 6;
    int max_fp_args = 8;
    int shadow_space = 0;
#endif

    bool has_hidden_retbuf = node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION);
#ifdef _WIN32
    int fixed_reg_args = nargs + (has_hidden_retbuf ? 1 : 0);
    int stack_args = fixed_reg_args > max_reg_args ? fixed_reg_args - max_reg_args : 0;
    int stack_pad = (stack_args & 1) ? 8 : 0;
    int stack_reserve = stack_args > 0 ? shadow_space + stack_args * 8 + stack_pad : 0;
#else
    int gp_reg_args = has_hidden_retbuf ? 1 : 0;
    int fp_reg_args = 0;
    int stack_args = 0;
    for (int i = 0; i < nargs; i++) {
        arg_regs[i] = -1;
        arg_sizes[i] = argv[i]->ty->size;
        arg_is_float[i] = is_flonum(argv[i]->ty);
        arg_gp_idx[i] = -1;
        arg_fp_idx[i] = -1;
        arg_stack_idx[i] = -1;

        if (arg_is_float[i]) {
            if (fp_reg_args < max_fp_args)
                arg_fp_idx[i] = fp_reg_args++;
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
    int stack_reserve = stack_args > 0 ? shadow_space + stack_args * 8 + stack_pad : 0;
#endif

    int saved_scratch = used_regs & 3;
    if ((saved_scratch & 1) && hidden_ret_reg != 0) {
        printf("  mov [rbp-%d], r10\n", SPILL_R10);
        used_regs &= ~1;
    }
    if ((saved_scratch & 2) && hidden_ret_reg != 1) {
        printf("  mov [rbp-%d], r11\n", SPILL_R11);
        used_regs &= ~2;
    }

#ifdef _WIN32
    int reg_nargs = nargs < max_reg_args - (has_hidden_retbuf ? 1 : 0) ? nargs : max_reg_args - (has_hidden_retbuf ? 1 : 0);
    for (int i = 0; i < reg_nargs; i++) {
        arg_regs[i] = gen(argv[i]);
        arg_sizes[i] = argv[i]->ty->size;
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

    if (stack_reserve > 0)
        printf("  sub rsp, %d\n", stack_reserve);

    for (int i = nargs - 1; i >= 0; i--) {
        if (arg_stack_idx[i] < 0)
            continue;
        int r;
        if ((argv[i]->ty->kind == TY_STRUCT || argv[i]->ty->kind == TY_UNION) && argv[i]->ty->size > 8)
            r = gen_addr(argv[i]);
        else
            r = gen(argv[i]);
        if (is_flonum(argv[i]->ty)) {
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + arg_stack_idx[i] * 8, reg64[r]);
        } else {
            if (argv[i]->ty->size == 1)
                printf("  movzx %s, %s\n", reg64[r], reg8[r]);
            else if (argv[i]->ty->size == 4)
                printf("  mov %s, %s\n", reg32[r], reg32[r]);
            printf("  mov qword ptr [rsp+%d], %s\n", shadow_space + arg_stack_idx[i] * 8, reg64[r]);
        }
        free_reg(r);
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
    if (has_hidden_retbuf) {
        if (hidden_ret_reg == -1) {
            temp_ret_reg = alloc_reg();
            printf("  sub rsp, %d\n", node->ty->size);
            printf("  mov %s, rsp\n", reg64[temp_ret_reg]);
            hidden_ret_reg = temp_ret_reg;
        }
        printf("  mov %s, %s\n", argreg64[0], reg64[hidden_ret_reg]);
    }

#ifdef _WIN32
    for (int i = 0; i < reg_nargs; i++) {
        int argi = i + (has_hidden_retbuf ? 1 : 0);
        if (arg_is_float[i]) {
#ifdef _WIN32
            printf("  movq %s, %s\n", argxmm[argi], reg64[arg_regs[i]]);
            printf("  mov %s, %s\n", argreg64[argi], reg64[arg_regs[i]]);
#else
            printf("  movq %s, %s\n", argxmm[argi], reg64[arg_regs[i]]);
#endif
        } else if (arg_sizes[i] == 1) {
            printf("  movzx %s, %s\n", argreg64[argi], reg8[arg_regs[i]]);
        } else if (arg_sizes[i] == 4) {
            printf("  mov %s, %s\n", argreg32[argi], reg(arg_regs[i], 4));
        } else {
            printf("  mov %s, %s\n", argreg64[argi], reg(arg_regs[i], 8));
        }
        free_reg(arg_regs[i]);
    }
#else
    for (int i = 0; i < nargs; i++) {
        if (arg_stack_idx[i] >= 0)
            continue;
        if (arg_is_float[i]) {
            printf("  movq %s, %s\n", argxmm[arg_fp_idx[i]], reg64[arg_regs[i]]);
        } else if (arg_sizes[i] == 1) {
            printf("  movzx %s, %s\n", argreg64[arg_gp_idx[i]], reg8[arg_regs[i]]);
        } else if (arg_sizes[i] == 4) {
            printf("  mov %s, %s\n", argreg32[arg_gp_idx[i]], reg(arg_regs[i], 4));
        } else {
            printf("  mov %s, %s\n", argreg64[arg_gp_idx[i]], reg(arg_regs[i], 8));
        }
        free_reg(arg_regs[i]);
    }
#endif

#ifndef _WIN32
    printf("  mov eax, %d\n", xmm_args);
#endif
    if (call_target) {
        emit_direct_call(call_target);
    } else {
        int callee = gen(node->lhs);
        printf("  call %s\n", reg64[callee]);
        free_reg(callee);
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
        if (temp_ret_reg != -1)
            return temp_ret_reg;
        return hidden_ret_reg;
    }

    int r = alloc_reg();
    if (node->ty && is_flonum(node->ty)) {
        printf("  movq %s, xmm0\n", reg64[r]);
    } else {
        printf("  mov %s, rax\n", reg64[r]);
    }
    return r;
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

static char *var_label(LVar *var) {
    return var->asm_name ? var->asm_name : var->name;
}

static char *reg(int r, int size) {
    if (size == 1) return reg8[r];
    if (size == 2) return reg16[r];
    if (size == 4) return reg32[r];
    return reg64[r];
}

static char *ptr_size(int size) {
    if (size == 1) return "byte ptr";
    if (size == 2) return "word ptr";
    if (size == 4) return "dword ptr";
    return "qword ptr";
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
}

static int alloc_reg(void) {
    for (int i = 0; i < 8; i++) {
        if ((used_regs & (1 << i)) == 0) {
            used_regs |= (1 << i);
            ever_used_regs |= (1 << i);
            return i;
        }
    }
    error("Register exhaustion");
    return 0;
}

static void free_reg(int i) {
    used_regs &= ~(1 << i);
}

static int gen(Node *node);

// Generate code to compute the absolute address of an lvalue.
static int gen_addr(Node *node) {
    switch (node->kind) {
    case ND_LVAR: {
        int r = alloc_reg();
        if (node->var->is_local)
            printf("  lea %s, [rbp-%d]\n", reg64[r], node->var->offset);
        else
            printf("  lea %s, [rip + %s]\n", reg64[r], var_label(node->var));
        return r;
    }
    case ND_DEREF:
        return gen(node->lhs);
    case ND_MEMBER: {
        int r = gen_addr(node->lhs);
        printf("  add %s, %d\n", reg64[r], node->member->offset);
        return r;
    }
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
            printf("  cmp %s, %d\n", reg(r_lhs, sz), (int)cond->rhs->val);
        } else {
            int r_rhs = gen(cond->rhs);
            printf("  cmp %s, %s\n", reg(r_lhs, sz), reg(r_rhs, sz));
            free_reg(r_rhs);
        }
        free_reg(r_lhs);

        char *jmp = "";
        if (cond->kind == ND_EQ) jmp = "jne";
        else if (cond->kind == ND_NE)
            jmp = "je";
        else if (cond->kind == ND_LT)
            jmp = use_unsigned_cmp(cond) ? "jae" : "jge";
        else if (cond->kind == ND_LE)
            jmp = use_unsigned_cmp(cond) ? "ja" : "jg";

        printf("  %s %s\n", jmp, label);
        return;
    }

    int r = gen(cond);
    printf("  cmp %s, 0\n", reg(r, cond->ty->size));
    free_reg(r);
    printf("  je %s\n", label);
}

// Generate code for a given node.
static int gen(Node *node) {
    if (!node) return -1;

    switch (node->kind) {
    case ND_NUM: {
        int r = alloc_reg();
        if (node->val == (int32_t)node->val) {
            printf("  mov %s, %lld\n", reg(r, op_size(node->ty)), (long long)node->val);
        } else {
            printf("  movabs %s, %lld\n", reg64[r], (long long)node->val);
        }
        return r;
    }
    case ND_FNUM: {
        int r = alloc_reg();
        int id = add_float_literal(node->fval, 8); // Always store as double
        printf("  movsd xmm0, [rip + .LF%d]\n", id);
        // Store float in integer register for now
        printf("  movq %s, xmm0\n", reg64[r]);
        return r;
    }
    case ND_LVAR: {
        int r = alloc_reg();
        char *label = var_label(node->var);
        if (node->var->ty->kind == TY_ARRAY) {
            if (node->var->is_local)
                printf("  lea %s, [rbp-%d]\n", reg64[r], node->var->offset);
            else
                printf("  lea %s, [rip + %s]\n", reg64[r], label);
        } else if (!node->var->is_local && node->var->is_function) {
            printf("  lea %s, [rip + %s]\n", reg64[r], label);
        } else if (is_flonum(node->var->ty)) {
            if (node->var->is_local) {
                if (node->var->ty->size == 4) {
                    printf("  movss xmm0, dword ptr [rbp-%d]\n", node->var->offset);
                    printf("  cvtss2sd xmm0, xmm0\n");
                } else {
                    printf("  movsd xmm0, qword ptr [rbp-%d]\n", node->var->offset);
                }
            } else {
                if (node->var->ty->size == 4) {
                    printf("  movss xmm0, dword ptr [rip + %s]\n", label);
                    printf("  cvtss2sd xmm0, xmm0\n");
                } else {
                    printf("  movsd xmm0, qword ptr [rip + %s]\n", label);
                }
            }
            printf("  movq %s, xmm0\n", reg64[r]);
        } else {
            if (node->var->is_local)
                emit_load(node->ty, r, format("[rbp-%d]", node->var->offset));
            else
                emit_load(node->ty, r, format("[rip + %s]", label));
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
            if (node->rhs->ty && (node->rhs->ty->kind == TY_STRUCT || node->rhs->ty->kind == TY_UNION || node->rhs->ty->kind == TY_ARRAY))
                src = gen_addr(node->rhs);
            else
                src = gen(node->rhs);
            printf("  mov rcx, %d\n", node->lhs->ty->size);
            printf(".L.copy.%d:\n", c);
            printf("  cmp rcx, 0\n");
            printf("  je .L.copy_end.%d\n", c);
            printf("  mov al, byte ptr [%s + rcx - 1]\n", reg64[src]);
            printf("  mov byte ptr [%s + rcx - 1], al\n", reg64[dst]);
            printf("  sub rcx, 1\n");
            printf("  jmp .L.copy.%d\n", c);
            printf(".L.copy_end.%d:\n", c);
            free_reg(src);
            return dst;
        }
        if (is_flonum(node->lhs->ty)) {
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
        }
        if (node->lhs->kind == ND_LVAR && node->lhs->var->is_local && node->lhs->var->ty->kind != TY_ARRAY) {
            if (node->rhs->kind == ND_ADD && node->rhs->lhs->kind == ND_LVAR &&
                node->rhs->lhs->var == node->lhs->var && node->rhs->rhs->kind == ND_NUM &&
                node->rhs->rhs->val == (int32_t)node->rhs->rhs->val) {
                printf("  add %s [rbp-%d], %d\n", ptr_size(node->lhs->ty->size), node->lhs->var->offset, (int)node->rhs->rhs->val);
                return -1;
            }
            if (node->rhs->kind == ND_NUM && node->rhs->val == (int32_t)node->rhs->val) {
                printf("  mov %s [rbp-%d], %d\n", ptr_size(node->lhs->ty->size), node->lhs->var->offset, (int)node->rhs->val);
                return -1;
            }
            int r2 = gen(node->rhs);
            printf("  mov [rbp-%d], %s\n", node->lhs->var->offset, reg(r2, node->lhs->ty->size));
            return r2;
        }
        // Bitfield assignment: read-modify-write
        if (node->lhs->kind == ND_MEMBER && node->lhs->member &&
            node->lhs->member->bit_width > 0) {
            int bw = node->lhs->member->bit_width;
            int bo = node->lhs->member->bit_offset;
            int unit_sz = node->lhs->member->ty->size;
            unsigned long long mask = ((1ULL << bw) - 1) << bo;

            // Check if RHS reads the same bitfield (compound assignment like s.x += 1)
            // In that case, we need to read the current value first
            bool rhs_reads_same = false;
            if (node->rhs->kind == ND_MEMBER && node->rhs->member == node->lhs->member) {
                rhs_reads_same = true;
            } else if (node->rhs->kind == ND_ADD || node->rhs->kind == ND_SUB ||
                       node->rhs->kind == ND_MUL || node->rhs->kind == ND_DIV ||
                       node->rhs->kind == ND_BITAND || node->rhs->kind == ND_BITOR ||
                       node->rhs->kind == ND_BITXOR) {
                // Check if any operand reads the same bitfield
                if (node->rhs->lhs && node->rhs->lhs->kind == ND_MEMBER &&
                    node->rhs->lhs->member == node->lhs->member)
                    rhs_reads_same = true;
                if (node->rhs->rhs && node->rhs->rhs->kind == ND_MEMBER &&
                    node->rhs->rhs->member == node->lhs->member)
                    rhs_reads_same = true;
            }

            // Generate RHS (the new value to assign)
            int r2 = gen(node->rhs);

            if (rhs_reads_same) {
                // Compound assignment: RHS reads the bitfield, so we need read-modify-write
                int ra = gen_addr(node->lhs);
                int rt = alloc_reg();
                // rt = old value from storage unit (full extraction)
                emit_load(node->lhs->member->ty, rt, format("[%s]", reg64[ra]));
                // Extract current bitfield value
                if (bo > 0) {
                    printf("  shr %s, %d\n", reg64[rt], bo);
                }
                if (bw < unit_sz * 8) {
                    unsigned long long m = (1ULL << bw) - 1;
                    printf("  movabs rax, %llu\n", m);
                    printf("  and %s, rax\n", reg64[rt]);
                }
                // Now r2 should contain the result of the compound operation (e.g., old+1)
                // But the gen() call above already evaluated the RHS which included reading
                // the bitfield with our normal bitfield read code - so r2 has the result
                // We just need to store it back with proper masking

                // Clear the bitfield bits in old value
                printf("  movabs rax, %llu\n", ~mask);
                printf("  and %s, rax\n", reg64[rt]);
                // Mask and shift new value into position
                int rv = alloc_reg();
                printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
                printf("  movabs rax, %llu\n", (1ULL << bw) - 1);
                printf("  and %s, rax\n", reg64[rv]);
                if (bo > 0)
                    printf("  shl %s, %d\n", reg64[rv], bo);
                printf("  or %s, %s\n", reg64[rt], reg64[rv]);
                // Store back
                if (unit_sz == 1)
                    printf("  mov byte ptr [%s], %s\n", reg64[ra], reg8[rt]);
                else if (unit_sz == 2)
                    printf("  mov word ptr [%s], %s\n", reg64[ra], reg(rt, 2));
                else if (unit_sz == 4)
                    printf("  mov dword ptr [%s], %s\n", reg64[ra], reg(rt, 4));
                else
                    printf("  mov qword ptr [%s], %s\n", reg64[ra], reg64[rt]);
                free_reg(rv);
                free_reg(rt);
                free_reg(ra);
                return r2;
            }

            // Original simple assignment code
            int r1 = gen_addr(node->lhs);
            // Load current storage unit
            emit_load(node->lhs->member->ty, r1, format("[%s]", reg64[r1]));
            // We used r1 for loading, but we need the address again.
            // Re-generate address.
            free_reg(r1);
            int ra = gen_addr(node->lhs);
            int rt = alloc_reg();
            // rt = old value from storage unit
            emit_load(node->lhs->member->ty, rt, format("[%s]", reg64[ra]));
            // Clear the bitfield bits in old value
            printf("  movabs rax, %llu\n", ~mask);
            printf("  and %s, rax\n", reg64[rt]);
            // Mask and shift new value into position
            int rv = alloc_reg();
            printf("  mov %s, %s\n", reg64[rv], reg64[r2]);
            printf("  movabs rax, %llu\n", (1ULL << bw) - 1);
            printf("  and %s, rax\n", reg64[rv]);
            if (bo > 0)
                printf("  shl %s, %d\n", reg64[rv], bo);
            printf("  or %s, %s\n", reg64[rt], reg64[rv]);
            // Store back
            if (unit_sz == 1)
                printf("  mov byte ptr [%s], %s\n", reg64[ra], reg8[rt]);
            else if (unit_sz == 2)
                printf("  mov word ptr [%s], %s\n", reg64[ra], reg(rt, 2));
            else if (unit_sz == 4)
                printf("  mov dword ptr [%s], %s\n", reg64[ra], reg(rt, 4));
            else
                printf("  mov qword ptr [%s], %s\n", reg64[ra], reg64[rt]);
            free_reg(rv);
            free_reg(rt);
            free_reg(ra);
            return r2;
        }
        int r1 = gen_addr(node->lhs);
        int r2 = gen(node->rhs);
        printf("  mov [%s], %s\n", reg64[r1], reg(r2, node->lhs->ty->size));
        free_reg(r1);
        return r2;
    }
    case ND_NEG: {
        int r = gen(node->lhs);
        if (is_flonum(node->ty)) {
            // Negate float: xor with sign bit
            printf("  movq xmm0, %s\n", reg64[r]);
            // Use pxor with sign bit mask
            printf("  movabs %s, %lld\n", reg64[r], (long long)0x8000000000000000LL);
            printf("  movq xmm1, %s\n", reg64[r]);
            printf("  xorpd xmm0, xmm1\n");
            printf("  movq %s, xmm0\n", reg64[r]);
        } else {
            printf("  neg %s\n", reg(r, op_size(node->ty)));
        }
        return r;
    }
    case ND_NOT: {
        int r = gen(node->lhs);
        printf("  cmp %s, 0\n", reg(r, op_size(node->lhs->ty)));
        printf("  sete al\n");
        printf("  movzx %s, al\n", reg(r, 4));
        return r;
    }
    case ND_POST_INC:
    case ND_POST_DEC: {
        int r = gen_addr(node->lhs);
        int r2 = alloc_reg();
        int sz = node->lhs->ty->size;
        printf("  mov %s, %s [%s]\n", reg(r2, sz), ptr_size(sz), reg64[r]);
        int delta = 1;
        if (node->lhs->ty->kind == TY_PTR || node->lhs->ty->kind == TY_ARRAY)
            delta = node->lhs->ty->base->size;
        if (node->kind == ND_POST_INC)
            printf("  add %s [%s], %d\n", ptr_size(sz), reg64[r], delta);
        else
            printf("  sub %s [%s], %d\n", ptr_size(sz), reg64[r], delta);
        free_reg(r);
        return r2;
    }
    case ND_MEMBER: {
        int r = gen_addr(node);
        if (node->ty->kind == TY_ARRAY) {
            return r; // array decays to pointer
        }
        emit_load(node->ty, r, format("[%s]", reg64[r]));
        // Bitfield: extract the relevant bits
        if (node->member && node->member->bit_width > 0) {
            int bw = node->member->bit_width;
            int bo = node->member->bit_offset;
            int unit_bits = node->member->ty->size * 8;
            if (bo > 0)
                printf("  shr %s, %d\n", reg64[r], bo);
            if (bw < unit_bits) {
                if (node->member->ty->is_unsigned) {
                    unsigned long long mask = (1ULL << bw) - 1;
                    printf("  movabs rax, %llu\n", mask);
                    printf("  and %s, rax\n", reg64[r]);
                } else {
                    // Sign-extend: shift left then arithmetic shift right
                    int shift = 64 - bw;
                    printf("  shl %s, %d\n", reg64[r], shift);
                    printf("  sar %s, %d\n", reg64[r], shift);
                }
            }
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
            // float→int: value is always double internally
            printf("  movq xmm0, %s\n", reg64[r]);
            if (to->size == 8 && to->is_unsigned) {
                int c = ++rcc_label_count;
                printf("  mov rax, 0x43e0000000000000\n");
                printf("  movq xmm1, rax\n");
                printf("  comisd xmm0, xmm1\n");
                printf("  jb .L.ucast.%d\n", c);
                printf("  subsd xmm0, xmm1\n");
                printf("  cvttsd2siq %s, xmm0\n", reg64[r]);
                printf("  mov rcx, 0x8000000000000000\n");
                printf("  or %s, rcx\n", reg64[r]);
                printf("  jmp .L.ucast_end.%d\n", c);
                printf(".L.ucast.%d:\n", c);
                printf("  cvttsd2siq %s, xmm0\n", reg64[r]);
                printf(".L.ucast_end.%d:\n", c);
            } else {
                printf("  cvttsd2si %s, xmm0\n", to->size == 8 ? reg64[r] : reg32[r]);
            }
        } else if (is_integer(from) && is_flonum(to)) {
            // int→float: always produce double internally
            printf("  cvtsi2sd xmm0, %s\n", from->size < 4 ? reg32[r] : reg(r, from->size));
            if (to->kind == TY_FLOAT) {
                printf("  cvtsd2ss xmm0, xmm0\n");
                printf("  cvtss2sd xmm0, xmm0\n");
            }
            printf("  movq %s, xmm0\n", reg64[r]);
        } else if (is_flonum(from) && is_flonum(to)) {
            if (to->kind == TY_FLOAT && from->kind != TY_FLOAT) {
                printf("  movq xmm0, %s\n", reg64[r]);
                printf("  cvtsd2ss xmm0, xmm0\n");
                printf("  cvtss2sd xmm0, xmm0\n");
                printf("  movq %s, xmm0\n", reg64[r]);
            }
        } else if (to->size == 1) {
            printf("  movsx %s, %s\n", reg32[r], reg8[r]);
        } else if (to->size == 2) {
            printf("  movsx %s, %s\n", reg32[r], reg16[r]);
        } else if (to->size == 4 && from->size == 8) {
            printf("  mov %s, %s\n", reg32[r], reg32[r]);
        }
        return r;
    }
    case ND_BITNOT: {
        int r = gen(node->lhs);
        printf("  not %s\n", reg(r, op_size(node->ty)));
        return r;
    }
    case ND_STR: {
        int r = alloc_reg();
        printf("  lea %s, [rip + .LC%d]\n", reg64[r], node->str_id);
        return r;
    }
    case ND_DEREF: {
        if (node->ty->kind == TY_FUNC || node->ty->kind == TY_ARRAY)
            return gen(node->lhs);
        int r = gen(node->lhs);
        emit_load(node->ty, r, format("[%s]", reg64[r]));
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
                free_reg(src);
            } else {
                int r = gen(node->lhs);
                if (node->lhs->ty && is_flonum(node->lhs->ty)) {
                    printf("  movq xmm0, %s\n", reg64[r]);
                } else {
                    printf("  mov rax, %s\n", reg64[r]);
                }
                free_reg(r);
            }
        }
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        printf("  jmp .L.return.%s\n", current_fn);
        return -1;
    }
    case ND_NULL:
        return -1;
    case ND_COMMA: {
        int r1 = gen(node->lhs);
        if (r1 != -1) free_reg(r1);
        return gen(node->rhs);
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
        printf("  cmp %s, 0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, 0\n", reg(r, 4));
        printf("  je .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, 0\n", reg(rhs, node->rhs->ty->size));
        printf("  setne al\n");
        printf("  movzx %s, al\n", reg(r, 4));
        free_reg(rhs);
        printf(".L.end.%d:\n", c);
        return r;
    }
    case ND_LOGOR: {
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int lhs = gen(node->lhs);
        printf("  cmp %s, 0\n", reg(lhs, node->lhs->ty->size));
        printf("  mov %s, 1\n", reg(r, 4));
        printf("  jne .L.end.%d\n", c);
        free_reg(lhs);
        int rhs = gen(node->rhs);
        printf("  cmp %s, 0\n", reg(rhs, node->rhs->ty->size));
        printf("  setne al\n");
        printf("  movzx %s, al\n", reg(r, 4));
        free_reg(rhs);
        printf(".L.end.%d:\n", c);
        return r;
    }
    case ND_COND: {
        int c = ++rcc_label_count;
        int r = alloc_reg();
        int cond = gen(node->cond);
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
            printf("  jmp %s\n", end_label);
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
        printf("  jmp %s\n", begin_label);
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
        printf("  cmp %s, 0\n", reg(r, node->cond->ty->size));
        free_reg(r);
        printf("  jne .L.begin.%d\n", c);
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
                printf(".L.skip.%d:\n", skip_lbl);
            } else {
                if (cs->case_val == (int32_t)cs->case_val) {
                    printf("  cmp %s, %lld\n", reg(cond, sz), (long long)cs->case_val);
                } else {
                    int tmp = alloc_reg();
                    printf("  movabs %s, %lld\n", reg64[tmp], (long long)cs->case_val);
                    printf("  cmp %s, %s\n", reg(cond, sz), reg(tmp, sz));
                    free_reg(tmp);
                }
                printf("  je .L.case.%d\n", cs->label_id);
            }
        }
        if (node->default_case) {
            if (!node->default_case->label_id)
                node->default_case->label_id = ++rcc_label_count;
            printf("  jmp .L.case.%d\n", node->default_case->label_id);
        } else {
            printf("  jmp .L.end.%d\n", c);
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
        printf("  jmp .L.end.%d\n", break_stack[ctrl_depth - 1]);
        return -1;
    case ND_CONTINUE:
        if (ctrl_depth == 0)
            error("stray continue");
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        printf("  jmp .L.continue.%d\n", continue_stack[ctrl_depth - 1]);
        return -1;
    case ND_GOTO:
        emit_cleanup_range(node->cleanup_begin, node->cleanup_end);
        printf("  jmp .L.label.%s.%s\n", current_fn, node->label_name);
        return -1;
    case ND_LABEL: {
        printf(".L.label.%s.%s:\n", current_fn, node->label_name);
        return gen(node->lhs);
    }
    default:
        break;
    }

    int r_lhs = gen(node->lhs);

    // Float binary operations (must come before integer ops)
    if (is_flonum(node->ty) && (node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_MUL || node->kind == ND_DIV)) {
        int r_rhs = gen(node->rhs);
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
        return r_lhs;
    }

    // Float comparisons
    if ((node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) &&
        node->lhs->ty && node->rhs->ty && (is_flonum(node->lhs->ty) || is_flonum(node->rhs->ty))) {
        int r_rhs = gen(node->rhs);
        printf("  movq xmm0, %s\n", reg64[r_lhs]);
        printf("  movq xmm1, %s\n", reg64[r_rhs]);
        free_reg(r_rhs);
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
        return r_lhs;
    }

    // Fused Division/Modulo Optimization
    if (node->kind == ND_DIV || node->kind == ND_MOD) {
        int sz = op_size(node->ty);
        bool is_unsigned = use_unsigned(node->ty);
        int r_rhs = gen(node->rhs);
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
        free_reg(r_rhs);

        printf("  mov %s, %s\n", reg(r_lhs, sz), node->kind == ND_DIV ? (sz == 8 ? "rax" : "eax") : (sz == 8 ? "rdx" : "edx"));
        return r_lhs;
    }

    // Binary operators with potential immediate/memory optimization for RHS
    if (node->kind == ND_SHL || node->kind == ND_SHR) {
        int sz = op_size(node->ty);
        if (node->rhs->kind == ND_NUM) {
            printf("  %s %s, %d\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar"), reg(r_lhs, sz), (int)node->rhs->val);
        } else {
            int r_rhs = gen(node->rhs);
            printf("  mov ecx, %s\n", reg(r_rhs, 4));
            printf("  %s %s, cl\n", node->kind == ND_SHL ? "shl" : (use_unsigned(node->ty) ? "shr" : "sar"), reg(r_lhs, sz));
            free_reg(r_rhs);
        }
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
            inst = "imul";
        else if (node->kind == ND_BITAND)
            inst = "and";
        else if (node->kind == ND_BITXOR)
            inst = "xor";
        else if (node->kind == ND_BITOR)
            inst = "or";
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
            }
        } else {
            int r_rhs = gen(node->rhs);
            printf("  %s %s, %s\n", inst, reg(r_lhs, sz), reg(r_rhs, sz));
            free_reg(r_rhs);
        }

        if (node->kind == ND_EQ || node->kind == ND_NE || node->kind == ND_LT || node->kind == ND_LE) {
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
    all_funcs = prog->funcs;
    // Assembly header
    printf(".intel_syntax noprefix\n");
#ifndef _WIN32
    printf(".section .note.GNU-stack,\"\",@progbits\n");
#endif

    // Emit data section for strings
    if (prog->globals || prog->strs || float_lits) {
        printf("\n.data\n");
        for (LVar *var = prog->globals; var; var = var->next) {
            if (var->is_extern)
                continue;
            char *label = var->asm_name ? var->asm_name : var->name;
            if (var->ty->align > 1)
                printf("  .balign %d\n", var->ty->align);
            if (!var->is_static)
                printf(".globl %s\n", label);
            printf("%s:\n", label);
            if (var->init_data || var->relocs) {
                int pos = 0;
                for (Reloc *rel = var->relocs; rel; rel = rel->next) {
                    for (; pos < rel->offset; pos++)
                        printf("  .byte %d\n", (unsigned char)var->init_data[pos]);
                    if (rel->addend)
                        printf("  .quad %s%+d\n", rel->label, rel->addend);
                    else
                        printf("  .quad %s\n", rel->label);
                    pos += 8;
                }
                for (; pos < var->init_size; pos++)
                    printf("  .byte %d\n", (unsigned char)var->init_data[pos]);
                if (var->ty->size > var->init_size)
                    printf("  .zero %d\n", var->ty->size - var->init_size);
            } else if (var->has_init) {
                if (var->ty->size == 1)
                    printf("  .byte %lld\n", (long long)var->init_val);
                else if (var->ty->size == 4)
                    printf("  .long %lld\n", (long long)var->init_val);
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
                for (int i = 0; s->str[i]; i++) {
                    printf("  .byte %d\n", s->str[i]);
                }
                printf("  .byte 0\n"); // null terminator
            }
        }
        printf("\n.text\n");
    }

    for (Function *fn = prog->funcs; fn; fn = fn->next) {
        current_fn = fn->name;
        current_fn_def = fn;

        // Pass 1: Generate function body to temp file to discover register usage
        FILE *body_file = tmpfile();
        cg_stream = body_file;
        used_regs = 0;
        ever_used_regs = 0;
        ctrl_depth = 0;

        // Save params to locals (emitted to body buffer, will be after prologue)
#ifdef _WIN32
        char *param_regs64[] = {"rcx", "rdx", "r8", "r9"};
        char *param_regs32[] = {"ecx", "edx", "r8d", "r9d"};
        char *param_regs16[] = {"cx", "dx", "r8w", "r9w"};
        char *param_regs8[] = {"cl", "dl", "r8b", "r9b"};
        int max_param_regs = 4;
#else
        char *param_regs64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
        char *param_regs32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
        char *param_regs16[] = {"di", "si", "dx", "cx", "r8w", "r9w"};
        char *param_regs8[] = {"dil", "sil", "dl", "cl", "r8b", "r9b"};
        char *param_xmm[] = {"xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7"};
        int max_param_regs = 6;
        int max_param_xmm = 8;
        int param_xmm_index = 0;
#endif
        int param_index = fn->ty->return_ty && (fn->ty->return_ty->kind == TY_STRUCT || fn->ty->return_ty->kind == TY_UNION) ? 1 : 0;
        for (LVar *var = fn->params; var; var = var->param_next) {
#ifdef _WIN32
            if (param_index < max_param_regs) {
                char *preg = var->ty->size == 1 ? param_regs8[param_index]
                    : var->ty->size == 2        ? param_regs16[param_index]
                    : var->ty->size <= 4        ? param_regs32[param_index]
                                                : param_regs64[param_index];
                printf("  mov [rbp-%d], %s\n", var->offset, preg);
                param_index++;
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
                }
            } else if (param_index < max_param_regs) {
                char *preg = var->ty->size == 1 ? param_regs8[param_index]
                    : var->ty->size == 2        ? param_regs16[param_index]
                    : var->ty->size <= 4        ? param_regs32[param_index]
                                                : param_regs64[param_index];
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && var->ty->size > 8) {
                    int c = ++rcc_label_count;
                    printf("  mov rcx, %d\n", var->ty->size);
                    printf(".L.pcopy.%d:\n", c);
                    printf("  cmp rcx, 0\n");
                    printf("  je .L.pcopy_end.%d\n", c);
                    printf("  mov al, byte ptr [%s + rcx - 1]\n", preg);
                    printf("  mov byte ptr [rbp-%d + rcx - 1], al\n", var->offset);
                    printf("  sub rcx, 1\n");
                    printf("  jmp .L.pcopy.%d\n", c);
                    printf(".L.pcopy_end.%d:\n", c);
                } else {
                    printf("  mov [rbp-%d], %s\n", var->offset, preg);
                }
                param_index++;
            }
#endif
        }

        for (Node *n = fn->body; n; n = n->next) {
            int r = gen(n);
            if (r != -1) free_reg(r);
        }
        fflush(body_file);

        // Pass 2: Emit optimized prologue, body, epilogue to stdout
        cg_stream = stdout;

        // Determine which callee-saved regs need saving (indices 2-7)
        int callee_mask = ever_used_regs >> 2; // bits 0-5 = rbx,r12,r13,r14,r15,rsi
        int n_pushes = 0;
        for (int j = 0; j < 6; j++)
            if (callee_mask & (1 << j)) n_pushes++;

        // Calculate stack frame size
        // Total space below rbp must cover: locals + spills + shadow (32)
        int need = fn->stack_size + 32;
        int push_bytes = n_pushes * 8;
        int sub_amount = need - push_bytes;
        if (sub_amount < 32) sub_amount = 32;
        // Fix 16-byte alignment
        if ((push_bytes + sub_amount) % 16 != 0) sub_amount += 8;

        // Emit prologue
        printf(".globl %s\n", fn->name);
        printf("%s = %s\n", fn->name, fn->asm_name ? fn->asm_name : fn->name);
        printf("%s:\n", fn->asm_name ? fn->asm_name : fn->name);
        printf("  push rbp\n");
        printf("  mov rbp, rsp\n");

        // Only push callee-saved registers that were actually used
        for (int j = 0; j < 6; j++) {
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

        // Split into lines
        int line_cap = 1;
        for (char *q = body_text; *q; q++)
            if (*q == '\n')
                line_cap++;
        char **lines = malloc(sizeof(char *) * line_cap);
        int nlines = 0;
        char *p = body_text;
        while (*p) {
            lines[nlines] = p;
            char *nl = strchr(p, '\n');
            if (nl) {
                *nl = '\0';
                p = nl + 1;
            } else
                p += strlen(p);
            nlines++;
        }

        // Skip peephole on very large functions to avoid truncation/pathological compile behavior.
        if (opt_O0) goto skip_peep;
        if (nlines < 50000)
            for (int pass = 0; pass < 4; pass++) {
                for (int li = 0; li < nlines - 1; li++) {
                    if (!lines[li] || !lines[li][0]) continue;
                    int lj = li + 1;
                    while (lj < nlines && (!lines[lj] || !lines[lj][0])) lj++;
                    if (lj >= nlines) break;

                    char d1[80], s1[80], d2[80], s2[80];

                    // Pattern 1: mov REG, SRC; mov DEST, REG → mov DEST, SRC
                    // (copy propagation: eliminate temp register moves)
                    if (sscanf(lines[li], "  mov %[^,], %s", d1, s1) == 2 &&
                        sscanf(lines[lj], "  mov %[^,], %s", d2, s2) == 2 &&
                        strcmp(s2, d1) == 0 && is_reg(d1) && is_reg(s1)) {
                        char newline[200];
                        snprintf(newline, sizeof(newline), "  mov %s, %s", d2, s1);
                        lines[lj] = strdup(newline);
                        int pid = phys_reg_id(d1);
                        if (pid >= 0 && !reg_live_after(lines, nlines, lj, pid))
                            lines[li] = "";
                        continue;
                    }

                    // Pattern 2: mov [rbp-N], REG; mov REGx, {dword,qword} ptr [rbp-N]
                    // (store-load forwarding)
                    {
                        int off1, off2;
                        char sr[32], dr[32];
                        if (sscanf(lines[li], "  mov [rbp-%d], %s", &off1, sr) == 2) {
                            if (sscanf(lines[lj], "  mov %[^,], dword ptr [rbp-%d]", dr, &off2) == 2 &&
                                off1 == off2) {
                                const char *r32 = to32(sr);
                                if (r32) {
                                    char newline[160];
                                    snprintf(newline, sizeof(newline), "  mov %s, %s", dr, r32);
                                    lines[lj] = strdup(newline);
                                    continue;
                                }
                            }
                            if (sscanf(lines[lj], "  mov %[^,], qword ptr [rbp-%d]", dr, &off2) == 2 &&
                                off1 == off2) {
                                char newline[160];
                                snprintf(newline, sizeof(newline), "  mov %s, %s", dr, sr);
                                lines[lj] = strdup(newline);
                                continue;
                            }
                        }
                    }

                    // Pattern 2b: mov dword ptr [rbp-N], VAL; mov REGx, dword ptr [rbp-N]
                    {
                        int off1, off2, val;
                        char dr[32];
                        if (sscanf(lines[li], "  mov dword ptr [rbp-%d], %d", &off1, &val) == 2 &&
                            sscanf(lines[lj], "  mov %[^,], dword ptr [rbp-%d]", dr, &off2) == 2 &&
                            off1 == off2) {
                            char newline[160];
                            snprintf(newline, sizeof(newline), "  mov %s, %d", dr, val);
                            lines[lj] = strdup(newline);
                            continue;
                        }
                    }

                    // Pattern 3: jmp .LABEL; .LABEL: → delete jmp
                    {
                        char lbl1[80], lbl2[80];
                        if (sscanf(lines[li], "  jmp %s", lbl1) == 1 &&
                            sscanf(lines[lj], "%[^:]:", lbl2) == 1) {
                            char *t = lbl2;
                            while (*t == ' ') t++;
                            if (strcmp(lbl1, t) == 0) {
                                lines[li] = "";
                                continue;
                            }
                        }
                    }

                    // Pattern 4: mov REG, IMM; OP REG2, REG → OP REG2, IMM
                    // (fold immediate into cmp/add/sub/and/or/xor)
                    {
                        char rd[32];
                        int imm_val;
                        char op[16], od[64], os[32];
                        if (sscanf(lines[li], "  mov %[^,], %d", rd, &imm_val) == 2 && is_reg(rd)) {
                            // Skip hex values: sscanf %d parses only leading '0' of 0x...
                            char *comma = strchr(lines[li], ',');
                            if (comma) {
                                char *val = comma + 1;
                                while (*val == ' ' || *val == '\t') val++;
                                if (val[0] == '0' && (val[1] == 'x' || val[1] == 'X'))
                                    continue;
                            }
                            int rd_pid = phys_reg_id(rd);
                            if (sscanf(lines[lj], "  %s %[^,], %s", op, od, os) == 3 &&
                                same_phys(os, rd) &&
                                (!strcmp(op, "cmp") || !strcmp(op, "add") || !strcmp(op, "sub") ||
                                 !strcmp(op, "and") || !strcmp(op, "or") || !strcmp(op, "xor") ||
                                 !strcmp(op, "imul"))) {
                                // Fold and eliminate identity ops
                                if (((!strcmp(op, "add") || !strcmp(op, "sub") || !strcmp(op, "or") || !strcmp(op, "xor")) && imm_val == 0) ||
                                    (!strcmp(op, "imul") && imm_val == 1)) {
                                    lines[lj] = "";
                                } else {
                                    char newline[160];
                                    snprintf(newline, sizeof(newline), "  %s %s, %d", op, od, imm_val);
                                    lines[lj] = strdup(newline);
                                }
                                if (rd_pid >= 0 && !reg_live_after(lines, nlines, lj, rd_pid))
                                    lines[li] = "";
                                continue;
                            }
                        }
                    }

                    // Pattern 5: 3-line chain: mov R, [mem]; OP R, IMM; mov dst, R → mov dst, [mem]; OP dst, IMM
                    {
                        int lk = lj + 1;
                        while (lk < nlines && (!lines[lk] || !lines[lk][0])) lk++;
                        if (lk < nlines) {
                            char r1[32], mem1[128], op2[16], r2[32], imm2[32], d3[32], r3[32];
                            if (sscanf(lines[li], "  mov %[^,], %[^\n]", r1, mem1) == 2 &&
                                sscanf(lines[lj], "  %s %[^,], %s", op2, r2, imm2) == 3 &&
                                sscanf(lines[lk], "  mov %[^,], %s", d3, r3) == 2 &&
                                strcmp(r1, r2) == 0 && strcmp(r2, r3) == 0 &&
                                is_reg(d3) && !is_reg(mem1) &&
                                (!strcmp(op2, "add") || !strcmp(op2, "sub"))) {
                                char nl1[200], nl2[100];
                                snprintf(nl1, sizeof(nl1), "  mov %s, %s", d3, mem1);
                                snprintf(nl2, sizeof(nl2), "  %s %s, %s", op2, d3, imm2);
                                lines[li] = strdup(nl1);
                                lines[lj] = strdup(nl2);
                                lines[lk] = "";
                                continue;
                            }
                        }
                    }
                }
            }

    skip_peep:
        // Emit optimized lines
        for (int li = 0; li < nlines; li++) {
            if (lines[li] && lines[li][0])
                fprintf(stdout, "%s\n", lines[li]);
        }
        // FIXME: this leaks all strdup'ed peep-optimized lines
        free(lines);
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
        printf("  add rsp, %d\n", sub_amount);
        for (int j = 5; j >= 0; j--) {
            if (callee_mask & (1 << j))
                printf("  pop %s\n", reg64[j + 2]);
        }
        printf("  pop rbp\n");
        printf("  ret\n");
    }

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
