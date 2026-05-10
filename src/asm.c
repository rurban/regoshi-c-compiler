// SPDX-License-Identifier: LGPL-2.1-or-later
// Built-in assembler: parse rcc-generated .s text → ObjFile → ELF/Mach-O.
// Handles the exact subset of ARM64 and x86-64 assembly that rcc emits.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "asm.h"
#include "obj.h"
#include "arm64_enc.h"
#include "x86_enc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    ObjFile *obj;
    int cur_sec; // current section: SEC_TEXT / SEC_DATA / SEC_BSS / SEC_RODATA
    int lineno;
    const char *filename;

    // Backpatching: forward label references
    // (max 512 pending fixups at once; sufficient for compiler output)
    struct Fixup {
        size_t patch_off; // byte offset in section buffer
        int section;
        char label[128];
        int kind; // FIXUP_ARM64_B26, FIXUP_ARM64_B19, FIXUP_REL32
        int64_t addend;
    } fixups[512];
    int nfixups;

    // Local label map (for forward references: .Lxxx → offset)
    struct LocalSym {
        char name[128];
        int section;
        size_t offset;
    } locals[2048];
    int nlocals;
} AsmState;

static void asm_error(AsmState *as, const char *msg) {
    fprintf(stderr, "%s:%d: asm error: %s\n",
            as->filename ? as->filename : "?", as->lineno, msg);
}

// Current section buffer
static SecBuf *cur_sec_buf(AsmState *as) {
    switch (as->cur_sec) {
    case SEC_TEXT: return &as->obj->text;
    case SEC_DATA: return &as->obj->data;
    case SEC_RODATA: return &as->obj->rodata;
    default: return &as->obj->data; // BSS handled specially
    }
}

static size_t cur_off(AsmState *as) {
    if (as->cur_sec == SEC_BSS) return as->obj->bss_size;
    return cur_sec_buf(as)->len;
}

// ---------------------------------------------------------------------------
// Symbol management
// ---------------------------------------------------------------------------
static void define_label(AsmState *as, const char *name, bool is_global, bool is_weak,
                         bool is_func) {
    int sec = as->cur_sec;
    size_t off = cur_off(as);

    // Check if already in symbol table (global declaration first, then definition)
    int idx = objfile_find_sym(as->obj, name);
    if (idx < 0) {
        SymBind bind = is_weak ? SB_WEAK : (is_global ? SB_GLOBAL : SB_LOCAL);
        SymType type = is_func ? ST_FUNC : ST_OBJECT;
        idx = objfile_add_sym(as->obj, name, sec, off, 0, bind, type);
    } else {
        as->obj->syms[idx].section = sec;
        as->obj->syms[idx].offset = off;
        if (is_global && as->obj->syms[idx].bind == SB_LOCAL)
            as->obj->syms[idx].bind = is_weak ? SB_WEAK : SB_GLOBAL;
        if (is_func) as->obj->syms[idx].type = ST_FUNC;
    }
    (void)idx;

    // Also record as local sym for backpatching
    if (as->nlocals < 2047) {
        struct LocalSym *ls = &as->locals[as->nlocals++];
        strncpy(ls->name, name, sizeof(ls->name) - 1);
        ls->name[sizeof(ls->name) - 1] = '\0';
        ls->section = sec;
        ls->offset = off;
    }

    // Resolve any pending fixups for this label
    for (int i = 0; i < as->nfixups; i++) {
        struct Fixup *fx = &as->fixups[i];
        if (strcmp(fx->label, name) != 0) continue;
        if (fx->section != sec) {
            asm_error(as, "cross-section fixup");
            continue;
        }
        SecBuf *buf = cur_sec_buf(as);
        int64_t target = (int64_t)off + fx->addend;
        int64_t pc = (int64_t)fx->patch_off;
        switch (fx->kind) {
        case FIXUP_ARM64_B26: {
            int32_t delta = (int32_t)((target - pc) / 4);
            uint32_t old;
            memcpy(&old, buf->data + fx->patch_off, 4);
            old = (old & ~0x03ffffffu) | ((uint32_t)delta & 0x03ffffffu);
            secbuf_patch32le(buf, fx->patch_off, old);
            break;
        }
        case FIXUP_ARM64_B19: {
            int32_t delta = (int32_t)((target - pc) / 4);
            uint32_t old;
            memcpy(&old, buf->data + fx->patch_off, 4);
            old = (old & ~(0x7ffff << 5)) | (((uint32_t)delta & 0x7ffff) << 5);
            secbuf_patch32le(buf, fx->patch_off, old);
            break;
        }
        case FIXUP_REL32: {
            // 32-bit PC-relative: target - (patch_off + 4)
            int32_t delta = (int32_t)(target - (pc + 4));
            secbuf_patch32le(buf, fx->patch_off, (uint32_t)delta);
            break;
        }
        default: break;
        }
        // Remove this fixup
        as->fixups[i] = as->fixups[--as->nfixups];
        i--;
    }
}

// Add a fixup for a forward-referenced label
static void add_fixup(AsmState *as, size_t patch_off, int section,
                      const char *label, int kind, int64_t addend) {
    if (as->nfixups >= 511) {
        asm_error(as, "too many fixups");
        return;
    }
    struct Fixup *fx = &as->fixups[as->nfixups++];
    fx->patch_off = patch_off;
    fx->section = section;
    strncpy(fx->label, label, sizeof(fx->label) - 1);
    fx->label[sizeof(fx->label) - 1] = '\0';
    fx->kind = kind;
    fx->addend = addend;
}

// Look up a label offset (returns -1 if not found)
static int64_t lookup_local(AsmState *as, const char *name, int *sec_out) {
    for (int i = as->nlocals - 1; i >= 0; i--) {
        if (strcmp(as->locals[i].name, name) == 0) {
            if (sec_out) *sec_out = as->locals[i].section;
            return (int64_t)as->locals[i].offset;
        }
    }
    return -1;
}

// Ensure a symbol is in the object's symbol table (for extern refs in relocs)
static int ensure_sym(AsmState *as, const char *name) {
    int idx = objfile_find_sym(as->obj, name);
    if (idx < 0)
        idx = objfile_add_sym(as->obj, name, SEC_UNDEF, 0, 0, SB_GLOBAL, ST_NOTYPE);
    return idx;
}

// ---------------------------------------------------------------------------
// Text parsing helpers
// ---------------------------------------------------------------------------
static char *skip_ws(char *p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static char *trim_end(char *p) {
    int len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '\n' || p[len - 1] == '\r')) {
        p[--len] = '\0';
    }
    return p;
}

// Parse a comma-separated operand list. Returns count, fills ops[].
// Each ops[i] points into a copy of the operand string (null-terminated).
static int split_operands(char *line, char **ops, int max_ops) {
    int n = 0;
    char *p = line;
    while (*p && n < max_ops) {
        p = skip_ws(p);
        if (!*p) break;
        ops[n++] = p;
        // Find next comma, but skip over brackets and quoted strings
        int depth = 0;
        while (*p) {
            if (*p == '[' || *p == '(') depth++;
            else if (*p == ']' || *p == ')')
                depth--;
            else if (*p == ',' && depth == 0) {
                *p++ = '\0';
                break;
            } else if (*p == '#' || *p == '$') { /* skip */
            }
            p++;
        }
    }
    // Trim each operand
    for (int i = 0; i < n; i++) {
        ops[i] = skip_ws(ops[i]);
        trim_end(ops[i]);
    }
    return n;
}

// ---------------------------------------------------------------------------
// ARM64 register parsing
// ---------------------------------------------------------------------------
static int parse_arm64_reg(const char *s, bool *is32) {
    if (!s) return -1;
    s = skip_ws((char *)s);
    bool w = false;
    if (s[0] == 'x') w = false;
    else if (s[0] == 'w')
        w = true;
    else if (strcmp(s, "sp") == 0) {
        if (is32) *is32 = false;
        return 31;
    } else if (strcmp(s, "xzr") == 0) {
        if (is32) *is32 = false;
        return 31;
    } else if (strcmp(s, "wzr") == 0) {
        if (is32) *is32 = true;
        return 31;
    } else if (s[0] == 'd') {
        if (is32) *is32 = false;
        return atoi(s + 1);
    } else if (s[0] == 's') {
        if (is32) *is32 = true;
        return atoi(s + 1);
    } else
        return -1;
    if (is32) *is32 = w;
    if (strcmp(s + 1, "zr") == 0) return 31;
    if (strcmp(s + 1, "29") == 0) return 29;
    if (strcmp(s + 1, "30") == 0) return 30;
    return atoi(s + 1);
}

// Parse #imm or imm (no #)
static int64_t parse_imm(const char *s) {
    if (!s) return 0;
    s = skip_ws((char *)s);
    if (*s == '#') s++;
    if (*s == '-') return -strtoll(s + 1, NULL, 0);
    return strtoll(s, NULL, 0);
}

// Parse ARM64 memory operand like "[x29, #-8]" or "[sp, #16]!" or "[sp], #16"
// Returns base register, fills *imm, *preindex, *postindex
static int parse_arm64_mem(const char *s, int64_t *imm, bool *pre, bool *post,
                           int *rn2) {
    if (!s) return -1;
    s = skip_ws((char *)s);
    *imm = 0;
    *pre = false;
    *post = false;
    if (rn2) *rn2 = -1;

    // Detect post-index: "[base], #off"
    char buf[256];
    strncpy(buf, s, 255);
    buf[255] = 0;
    char *close = strchr(buf, ']');
    if (close) {
        char *after = skip_ws(close + 1);
        if (*after == ',') {
            *post = true;
            *imm = parse_imm(skip_ws(after + 1));
        }
        if (*(close - 1) == '!') {
            *pre = true;
            *(close - 1) = 0;
        } else
            *close = 0;
    }
    char *inner = buf;
    if (*inner == '[') inner++;
    inner = skip_ws(inner);

    // Split base, optionally offset
    char *comma = strchr(inner, ',');
    if (comma) {
        *comma = 0;
        char *off_s = skip_ws(comma + 1);
        if (*off_s == 'x' || *off_s == 'w') {
            // Register offset
            if (rn2) *rn2 = parse_arm64_reg(off_s, NULL);
        } else if (!*post) {
            *imm = parse_imm(off_s);
        }
    }
    return parse_arm64_reg(inner, NULL);
}

// Extract relocation type from ":lo12:sym", ":got:sym", ":got_lo12:sym"
// Returns pointer to just the symbol name (modifies buf), sets *rel_type
static const char *parse_arm64_sym_reloc(const char *s, uint32_t *rel_type) {
    *rel_type = 0;
    if (s[0] == ':') {
        if (strncmp(s, ":lo12:", 6) == 0) {
            *rel_type = R_AARCH64_ADD_ABS_LO12_NC;
            return s + 6;
        }
        if (strncmp(s, ":got:", 5) == 0) {
            *rel_type = R_AARCH64_ADR_GOT_PAGE;
            return s + 5;
        }
        if (strncmp(s, ":got_lo12:", 10) == 0) {
            *rel_type = R_AARCH64_LD64_GOT_LO12_NC;
            return s + 10;
        }
    }
    // Darwin: sym@PAGE or sym@PAGEOFF
    char *at = strrchr(s, '@');
    if (at) {
        static char tmp[256];
        strncpy(tmp, s, sizeof(tmp) - 1);
        at = strrchr(tmp, '@');
        if (at) {
            *at = 0;
            if (strcmp(at + 1, "PAGE") == 0 || strcmp(at + 1, "GOTPAGE") == 0)
                *rel_type = R_AARCH64_ADR_PREL_PG_HI21;
            else if (strcmp(at + 1, "PAGEOFF") == 0 || strcmp(at + 1, "GOTPAGEOFF") == 0)
                *rel_type = R_AARCH64_ADD_ABS_LO12_NC;
            return tmp;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// x86-64 register parsing (AT&T names)
// ---------------------------------------------------------------------------
static X86Reg parse_x86_reg64(const char *s) {
    if (!s || *s != '%') return X86_NOREG;
    s++;
    if (!strcmp(s, "rax") || !strcmp(s, "eax") || !strcmp(s, "ax") || !strcmp(s, "al")) return X86_RAX;
    if (!strcmp(s, "rcx") || !strcmp(s, "ecx") || !strcmp(s, "cx") || !strcmp(s, "cl")) return X86_RCX;
    if (!strcmp(s, "rdx") || !strcmp(s, "edx") || !strcmp(s, "dx") || !strcmp(s, "dl")) return X86_RDX;
    if (!strcmp(s, "rbx") || !strcmp(s, "ebx") || !strcmp(s, "bx") || !strcmp(s, "bl")) return X86_RBX;
    if (!strcmp(s, "rsp") || !strcmp(s, "esp") || !strcmp(s, "sp") || !strcmp(s, "spl")) return X86_RSP;
    if (!strcmp(s, "rbp") || !strcmp(s, "ebp") || !strcmp(s, "bp") || !strcmp(s, "bpl")) return X86_RBP;
    if (!strcmp(s, "rsi") || !strcmp(s, "esi") || !strcmp(s, "si") || !strcmp(s, "sil")) return X86_RSI;
    if (!strcmp(s, "rdi") || !strcmp(s, "edi") || !strcmp(s, "di") || !strcmp(s, "dil")) return X86_RDI;
    if (!strcmp(s, "r8") || !strcmp(s, "r8d") || !strcmp(s, "r8w") || !strcmp(s, "r8b")) return X86_R8;
    if (!strcmp(s, "r9") || !strcmp(s, "r9d") || !strcmp(s, "r9w") || !strcmp(s, "r9b")) return X86_R9;
    if (!strcmp(s, "r10") || !strcmp(s, "r10d") || !strcmp(s, "r10w") || !strcmp(s, "r10b")) return X86_R10;
    if (!strcmp(s, "r11") || !strcmp(s, "r11d") || !strcmp(s, "r11w") || !strcmp(s, "r11b")) return X86_R11;
    if (!strcmp(s, "r12") || !strcmp(s, "r12d") || !strcmp(s, "r12w") || !strcmp(s, "r12b")) return X86_R12;
    if (!strcmp(s, "r13") || !strcmp(s, "r13d") || !strcmp(s, "r13w") || !strcmp(s, "r13b")) return X86_R13;
    if (!strcmp(s, "r14") || !strcmp(s, "r14d") || !strcmp(s, "r14w") || !strcmp(s, "r14b")) return X86_R14;
    if (!strcmp(s, "r15") || !strcmp(s, "r15w") || !strcmp(s, "r15b") || !strcmp(s, "r15")) return X86_R15;
    return X86_NOREG;
}

static int reg_size_x86(const char *s) {
    if (!s || *s != '%') return 8;
    s++;
    if (s[0] == 'r' && isdigit((unsigned char)s[1])) {
        // r8..r15: check suffix (e.g. r10d→4, r10w→2, r10b→1, r10→8)
        char *end = (char *)(s + 1);
        while (isdigit((unsigned char)*end)) end++;
        if (*end == 'd') return 4;
        if (*end == 'w') return 2;
        if (*end == 'b') return 1;
        return 8;
    }
    int n = strlen(s);
    if (n >= 2) {
        char last2 = s[n - 1];
        char first = s[0];
        if (first == 'r' && (s[1] == 'a' || s[1] == 'b' || s[1] == 'c' || s[1] == 'd' || s[1] == 's' || s[1] == 'd')) return 8;
        if (first == 'e') return 4;
        if (last2 == 'l' || last2 == 'h') return 1;
        if (last2 == 'x' && n == 2) return 2;
        if (last2 == 'p' || last2 == 'i') return 2; // sp, bp, si, di
    }
    return 8;
}

// Parse AT&T memory operand: disp(%base, %index, scale) or (%base)
// Returns true on success
static bool parse_x86_mem(const char *s, X86Mem *m) {
    m->base = X86_NOREG;
    m->index = X86_NOREG;
    m->scale = 1;
    m->disp = 0;

    char buf[256];
    strncpy(buf, s, 255);
    buf[255] = 0;
    char *paren = strchr(buf, '(');
    if (paren) {
        *paren = 0;
        if (buf[0]) {
            // Parse displacement (may be a symbol or integer)
            char *disp_s = skip_ws(buf);
            if (disp_s[0] == '-' || isdigit((unsigned char)disp_s[0]))
                m->disp = strtoll(disp_s, NULL, 0);
            // Symbol displacement handled separately
        }
        char *inner = paren + 1;
        char *close = strchr(inner, ')');
        if (close) *close = 0;

        char *parts[3] = {NULL, NULL, NULL};
        int np = 0;
        char *p = inner;
        while (*p && np < 3) {
            parts[np++] = skip_ws(p);
            char *c = strchr(p, ',');
            if (!c) break;
            *c = 0;
            p = c + 1;
        }
        if (np > 0) m->base = parse_x86_reg64(skip_ws(parts[0]));
        if (np > 1) m->index = parse_x86_reg64(skip_ws(parts[1]));
        if (np > 2) m->scale = (int)strtol(parts[2], NULL, 10);
        return true;
    }
    return false;
}

// Check if operand is a RIP-relative reference like "sym(%rip)".
// If so, fills *sym_out (static buffer) with the symbol name and returns true.
static bool is_rip_rel(const char *op, const char **sym_out) {
    static char sym_buf[256];
    const char *rip = strstr(op, "(%rip)");
    if (!rip) return false;
    int len = (int)(rip - op);
    if (len <= 0) return false; // bare (%rip) — unusual, skip
    strncpy(sym_buf, op, (size_t)len);
    sym_buf[len] = '\0';
    if (sym_out) *sym_out = sym_buf;
    return true;
}

// Emit an x86-64 RIP-relative instruction that takes a memory operand.
// The instruction is emitted with disp32=0; a R_X86_64_PC32 relocation
// is added pointing at sym with addend -4.
// encode_fn(buf, sz, dst_reg, mem) must emit the instruction.
// This variant handles LEA, MOV r,m, and similar load-from-RIP forms.
static void emit_rip_rel_load(AsmState *as, const char *sym,
                              void (*emit_fn)(SecBuf *, int, X86Reg, X86Mem),
                              int sz, X86Reg dst) {
    SecBuf *buf = &as->obj->text;
    X86Mem rip_mem = {X86_NOREG, X86_NOREG, 1, 0}; // base=NOREG → mod=0, rm=5 = RIP+disp32
    emit_fn(buf, sz, dst, rip_mem);
    int sidx = ensure_sym(as, sym);
    objfile_add_reloc(as->obj, SEC_TEXT, buf->len - 4, sidx, R_X86_64_PC32, -4);
}

// ---------------------------------------------------------------------------
// Directive handling
// ---------------------------------------------------------------------------
static void handle_directive(AsmState *as, const char *dir, char *args) {
    args = skip_ws(args);
    trim_end(args);

    if (!strcmp(dir, "text") || !strcmp(dir, "section__TEXT,__text") ||
        (strncmp(dir, "text", 4) == 0)) {
        as->cur_sec = SEC_TEXT;
    } else if (!strcmp(dir, "data") || !strcmp(dir, "section__DATA,__data")) {
        as->cur_sec = SEC_DATA;
    } else if (!strcmp(dir, "bss")) {
        as->cur_sec = SEC_BSS;
    } else if (!strcmp(dir, "rodata") || !strcmp(dir, "section.rodata")) {
        as->cur_sec = SEC_RODATA;
    } else if (!strncmp(dir, "section", 7)) {
        // .section .note.GNU-stack or similar — check for specific sections
        if (strstr(args, ".rodata") || strstr(args, "__const")) as->cur_sec = SEC_RODATA;
        else if (strstr(args, ".data"))
            as->cur_sec = SEC_DATA;
        else if (strstr(args, ".bss"))
            as->cur_sec = SEC_BSS;
        else if (strstr(args, ".text"))
            as->cur_sec = SEC_TEXT;
        // else: unknown section, ignore (e.g. .note.GNU-stack)
    } else if (!strcmp(dir, "globl") || !strcmp(dir, "global")) {
        // Mark symbol as global (may not be defined yet)
        char *sym = args;
        trim_end(sym);
        int idx = objfile_find_sym(as->obj, sym);
        if (idx < 0)
            idx = objfile_add_sym(as->obj, sym, SEC_UNDEF, 0, 0, SB_GLOBAL, ST_NOTYPE);
        else
            as->obj->syms[idx].bind = SB_GLOBAL;
    } else if (!strcmp(dir, "weak")) {
        char *sym = args;
        trim_end(sym);
        int idx = objfile_find_sym(as->obj, sym);
        if (idx < 0)
            objfile_add_sym(as->obj, sym, SEC_UNDEF, 0, 0, SB_WEAK, ST_NOTYPE);
        else
            as->obj->syms[idx].bind = SB_WEAK;
    } else if (!strcmp(dir, "type")) {
        // .type sym, @function / .type sym, %function
        char *sym = strtok(args, ",");
        if (!sym) return;
        char *kind = strtok(NULL, "");
        if (!kind) return;
        kind = skip_ws(kind);
        bool is_func = strstr(kind, "function") != NULL;
        int idx = objfile_find_sym(as->obj, sym);
        if (idx >= 0 && is_func) as->obj->syms[idx].type = ST_FUNC;
    } else if (!strcmp(dir, "size")) {
        // .size sym, .-sym (update symbol size)
        char *sym = strtok(args, ",");
        if (!sym) return;
        trim_end(sym);
        int idx = objfile_find_sym(as->obj, sym);
        if (idx >= 0 && as->obj->syms[idx].section == as->cur_sec) {
            size_t now = cur_off(as);
            as->obj->syms[idx].size = now - as->obj->syms[idx].offset;
        }
    } else if (!strcmp(dir, "set") || !strcmp(dir, "equiv")) {
        // .set alias, target
        char *alias = strtok(args, ",");
        if (!alias) return;
        char *target = strtok(NULL, "");
        if (!target) return;
        alias = skip_ws(alias);
        trim_end(alias);
        target = skip_ws(target);
        trim_end(target);
        // Create an alias symbol that points to the same location as target
        int tidx = ensure_sym(as, target);
        int aidx = objfile_find_sym(as->obj, alias);
        if (aidx < 0) {
            aidx = objfile_add_sym(as->obj, alias,
                                   as->obj->syms[tidx].section,
                                   as->obj->syms[tidx].offset,
                                   0, SB_GLOBAL, ST_NOTYPE);
        }
        (void)aidx;
    } else if (!strcmp(dir, "balign") || !strcmp(dir, "align") ||
               !strcmp(dir, "p2align")) {
        int a = atoi(args);
        if (!strcmp(dir, "p2align")) a = 1 << a;
        if (a > 1) {
            if (as->cur_sec == SEC_BSS) {
                size_t rem = as->obj->bss_size % (size_t)a;
                if (rem) as->obj->bss_size += (size_t)a - rem;
            } else {
                secbuf_align(cur_sec_buf(as), a);
            }
        }
    } else if (!strcmp(dir, "byte") || !strcmp(dir, "2byte") ||
               !strcmp(dir, "4byte") || !strcmp(dir, "hword") ||
               !strcmp(dir, "word") || !strcmp(dir, "long") ||
               !strcmp(dir, "quad") || !strcmp(dir, "octa") ||
               !strcmp(dir, "8byte")) {
        // Data emission
        int sz = (!strcmp(dir, "byte")) ? 1 : (!strcmp(dir, "2byte") || !strcmp(dir, "hword") || !strcmp(dir, "word")) ? 2
            : (!strcmp(dir, "4byte") || !strcmp(dir, "long"))                                                          ? 4
                                                                                                                       : 8;

        SecBuf *buf = cur_sec_buf(as);
        // May have multiple comma-separated values
        char *val = strtok(args, ",");
        while (val) {
            val = skip_ws(val);
            trim_end(val);
            // Check if it's a symbol reference (for relocation)
            bool is_sym = val[0] == '.' || isalpha((unsigned char)val[0]) || val[0] == '_';
            if (is_sym && sz == 8) {
                // Emit an absolute 64-bit relocation
                int64_t addend = 0;
                char symname[256];
                strncpy(symname, val, 255);
                // Check for +/-addend
                char *plus = strchr(symname, '+');
                char *minus = strchr(symname, '-');
                if (plus) {
                    addend = strtoll(plus, NULL, 0);
                    *plus = 0;
                } else if (minus) {
                    addend = strtoll(minus, NULL, 0);
                    *minus = 0;
                }
                size_t off = secbuf_emit64le(buf, (uint64_t)addend);
                int sidx = ensure_sym(as, symname);
                objfile_add_reloc(as->obj, as->cur_sec, off, sidx,
                                  R_AARCH64_ABS64, addend);
            } else if (!is_sym) {
                int64_t v = strtoll(val, NULL, 0);
                switch (sz) {
                case 1: secbuf_emit8(buf, (uint8_t)v); break;
                case 2: secbuf_emit16le(buf, (uint16_t)v); break;
                case 4: secbuf_emit32le(buf, (uint32_t)v); break;
                case 8: secbuf_emit64le(buf, (uint64_t)v); break;
                }
            }
            val = strtok(NULL, ",");
        }
    } else if (!strcmp(dir, "zero") || !strcmp(dir, "skip") || !strcmp(dir, "space")) {
        int n = atoi(args);
        if (as->cur_sec == SEC_BSS) {
            as->obj->bss_size += (size_t)n;
        } else {
            SecBuf *buf = cur_sec_buf(as);
            secbuf_reserve(buf, (size_t)n);
            memset(buf->data + buf->len, 0, (size_t)n);
            buf->len += (size_t)n;
        }
    } else if (!strcmp(dir, "ascii") || !strcmp(dir, "asciz") || !strcmp(dir, "string")) {
        SecBuf *buf = cur_sec_buf(as);
        // Find the content between quotes
        char *p = strchr(args, '"');
        if (!p) return;
        p++;
        while (*p && *p != '"') {
            if (*p == '\\') {
                p++;
                switch (*p) {
                case 'n': secbuf_emit8(buf, '\n'); break;
                case 't': secbuf_emit8(buf, '\t'); break;
                case 'r': secbuf_emit8(buf, '\r'); break;
                case '0': secbuf_emit8(buf, 0); break;
                case '\\': secbuf_emit8(buf, '\\'); break;
                case '"': secbuf_emit8(buf, '"'); break;
                default: secbuf_emit8(buf, (uint8_t)*p); break;
                }
            } else {
                secbuf_emit8(buf, (uint8_t)*p);
            }
            p++;
        }
        if (!strcmp(dir, "asciz") || !strcmp(dir, "string"))
            secbuf_emit8(buf, 0); // null terminator
    } else if (!strcmp(dir, "file") || !strcmp(dir, "loc") ||
               !strcmp(dir, "ident")) {
        // Debug info — ignore in binary output
    } else if (!strcmp(dir, "note") || !strncmp(dir, "note.", 5)) {
        // Ignore note sections
    }
    // Other directives (weak_reference, weak_definition, etc.) ignored
}

// ---------------------------------------------------------------------------
// ARM64 instruction encoding dispatch
// ---------------------------------------------------------------------------

// Map condition string → Arm64Cond
static Arm64Cond parse_arm64_cond(const char *s) {
    if (!strcmp(s, "eq")) return ARM64_EQ;
    if (!strcmp(s, "ne")) return ARM64_NE;
    if (!strcmp(s, "cs") || !strcmp(s, "hs")) return ARM64_CS;
    if (!strcmp(s, "cc") || !strcmp(s, "lo")) return ARM64_CC;
    if (!strcmp(s, "mi")) return ARM64_MI;
    if (!strcmp(s, "pl")) return ARM64_PL;
    if (!strcmp(s, "vs")) return ARM64_VS;
    if (!strcmp(s, "vc")) return ARM64_VC;
    if (!strcmp(s, "hi")) return ARM64_HI;
    if (!strcmp(s, "ls")) return ARM64_LS;
    if (!strcmp(s, "ge")) return ARM64_GE;
    if (!strcmp(s, "lt")) return ARM64_LT;
    if (!strcmp(s, "gt")) return ARM64_GT;
    if (!strcmp(s, "le")) return ARM64_LE;
    if (!strcmp(s, "al")) return ARM64_AL;
    return ARM64_AL;
}

// Encode a branch and handle relocation/backpatch
static void emit_arm64_branch(AsmState *as, uint32_t insn_base,
                              bool is_bl, bool is_cond, const char *target) {
    SecBuf *buf = &as->obj->text;
    size_t off = buf->len;

    // Try local label first
    int sec = 0;
    int64_t toff = lookup_local(as, target, &sec);
    if (toff >= 0 && sec == SEC_TEXT) {
        // Resolve now
        int32_t delta = (int32_t)((toff - (int64_t)off) / 4);
        if (is_cond)
            insn_base |= ((uint32_t)delta & 0x7ffff) << 5;
        else
            insn_base |= (uint32_t)delta & 0x3ffffff;
        secbuf_emit32le(buf, insn_base);
    } else {
        // Try global symbol
        int sidx = objfile_find_sym(as->obj, target);
        bool is_global_def = sidx >= 0 && as->obj->syms[sidx].section != SEC_UNDEF;

        secbuf_emit32le(buf, insn_base); // placeholder

        if (!is_global_def) {
            // Either forward local or external symbol → need fixup or reloc
            int idx = objfile_find_sym(as->obj, target);
            if (idx >= 0 && as->obj->syms[idx].section == SEC_UNDEF) {
                // External symbol → reloc
                uint32_t rtype = is_bl ? R_AARCH64_CALL26 : R_AARCH64_JUMP26;
                objfile_add_reloc(as->obj, SEC_TEXT, off, idx, rtype, 0);
            } else {
                // Forward local label → fixup
                add_fixup(as, off, SEC_TEXT, target,
                          is_cond ? FIXUP_ARM64_B19 : FIXUP_ARM64_B26, 0);
            }
        } else {
            // Already-defined global in text: patch directly
            int64_t tgt_off = (int64_t)as->obj->syms[sidx].offset;
            int32_t delta = (int32_t)((tgt_off - (int64_t)off) / 4);
            if (is_cond)
                insn_base |= ((uint32_t)delta & 0x7ffff) << 5;
            else
                insn_base |= (uint32_t)delta & 0x3ffffff;
            secbuf_patch32le(buf, off, insn_base);
        }
    }
}

// Encode ARM64 instruction line (mnemonic already separated, ops = operand string)
static bool encode_arm64(AsmState *as, const char *mnem, char *ops_str) {
    char ops_buf[512];
    strncpy(ops_buf, ops_str, 511);
    ops_buf[511] = 0;
    char *ops[6];
    int nops = split_operands(ops_buf, ops, 6);
    SecBuf *buf = &as->obj->text;

// Strip mnemonic suffix for condition codes in B.cond
// mnem is like "b.eq", "b.ne", etc. Already split at first '.'

// Helper macros
#define REG(n)  parse_arm64_reg(ops[n], NULL)
#define REG32(n, w) parse_arm64_reg(ops[n], w)
#define IMM(n)  parse_imm(ops[n])
#define SF(r)   ((r) < 32 ? 1 : 0)  // always 1 for x-regs in context

    bool is32_0 = false, is32_1 = false;
    int r0 = (nops > 0) ? parse_arm64_reg(ops[0], &is32_0) : -1;
    int r1 = (nops > 1) ? parse_arm64_reg(ops[1], &is32_1) : -1;
    int r2 = (nops > 2) ? parse_arm64_reg(ops[2], NULL) : -1;
    int sf = is32_0 ? 0 : 1;

    if (!strcmp(mnem, "nop")) {
        secbuf_emit32le(buf, arm64_nop());
        return true;
    }
    if (!strcmp(mnem, "ret")) {
        int rn = (nops > 0 && r0 >= 0) ? r0 : 30;
        secbuf_emit32le(buf, arm64_ret(rn));
        return true;
    }
    if (!strcmp(mnem, "br")) {
        secbuf_emit32le(buf, arm64_br(r0));
        return true;
    }
    if (!strcmp(mnem, "blr")) {
        secbuf_emit32le(buf, arm64_blr(r0));
        return true;
    }

    if (!strcmp(mnem, "bl")) {
        char *t = (nops > 0) ? ops[0] : ""; // target label
        uint32_t base = 0x94000000u;
        emit_arm64_branch(as, base, true, false, t);
        return true;
    }
    if (!strcmp(mnem, "b")) {
        if (nops > 0) {
            uint32_t base = 0x14000000u;
            emit_arm64_branch(as, base, false, false, ops[0]);
        }
        return true;
    }
    // B.cond: mnemonic is "b" and condition is in the rest after '.'
    // We handle it by checking if ops[0] is a label and mnem has '.'
    // (The caller should detect "b.XX" and pass mnem="b" and condition separately)
    // Here mnem might be "b.eq" etc.
    if (mnem[0] == 'b' && mnem[1] == '.') {
        Arm64Cond cond = parse_arm64_cond(mnem + 2);
        uint32_t base = 0x54000000u | cond;
        emit_arm64_branch(as, base, false, true, ops[0]);
        return true;
    }

    // CBZ / CBNZ
    if (!strcmp(mnem, "cbz") || !strcmp(mnem, "cbnz")) {
        bool nz = !strcmp(mnem, "cbnz");
        int rt = r0;
        char *lbl = (nops > 1) ? ops[1] : "";
        uint32_t base = (sf ? 0xb4000000u : 0x34000000u) | (nz ? 0x01000000u : 0) | rt;
        emit_arm64_branch(as, base, false, true, lbl);
        return true;
    }

    // MOV (immediate) → MOVZ
    if (!strcmp(mnem, "mov") && nops == 2) {
        if (ops[1][0] == '#' || isdigit((unsigned char)ops[1][0]) || ops[1][0] == '-') {
            // Immediate: emit movz (and possibly movk if > 16 bits)
            uint64_t val = (uint64_t)(int64_t)IMM(1);
            secbuf_emit32le(buf, arm64_movz(sf, r0, (uint16_t)(val & 0xffff), 0));
            if (val >> 16) {
                int sh = 16;
                uint64_t v = val >> 16;
                while (v && sh <= (sf ? 48 : 16)) {
                    secbuf_emit32le(buf, arm64_movk(sf, r0, (uint16_t)(v & 0xffff), sh));
                    v >>= 16;
                    sh += 16;
                }
            }
            return true;
        }
        // Register: MOV rd, rn → ORR rd, xzr, rn
        secbuf_emit32le(buf, arm64_orr_reg(sf, r0, 31, r1, ARM64_LSL, 0));
        return true;
    }

    if (!strcmp(mnem, "movz")) {
        // movz reg, #imm [, lsl #shift]
        int64_t imm = IMM(1);
        int shift = (nops > 2) ? (int)IMM(2) : 0; // e.g. "lsl #16"
        if (nops > 2 && strstr(ops[2], "lsl")) shift = (int)IMM(2);
        secbuf_emit32le(buf, arm64_movz(sf, r0, (uint16_t)imm, shift));
        return true;
    }
    if (!strcmp(mnem, "movk")) {
        int64_t imm = IMM(1);
        int shift = 0;
        if (nops > 2) {
            char *p = ops[2];
            while (*p && !isdigit((unsigned char)*p)) p++;
            shift = atoi(p);
        }
        secbuf_emit32le(buf, arm64_movk(sf, r0, (uint16_t)imm, shift));
        return true;
    }
    if (!strcmp(mnem, "mvn")) {
        secbuf_emit32le(buf, arm64_mvn(sf, r0, r1, ARM64_LSL, 0));
        return true;
    }

    // ADD / SUB
    if (!strcmp(mnem, "add") || !strcmp(mnem, "adds") ||
        !strcmp(mnem, "sub") || !strcmp(mnem, "subs")) {
        bool is_sub = (mnem[0] == 's' && mnem[1] == 'u');
        bool set_flags = (mnem[2] == 's' || (is_sub && mnem[3] == 's'));
        if (nops >= 3) {
            char *o2 = ops[2];
            bool is_imm = (o2[0] == '#' || isdigit((unsigned char)o2[0]));
            // Check for symbol reference (:lo12:sym)
            if (!is_imm && o2[0] == ':') {
                uint32_t rtype;
                const char *sym = parse_arm64_sym_reloc(o2, &rtype);
                uint32_t insn = arm64_add_imm(sf, r0, r1, 0, 0);
                size_t off = secbuf_emit32le(buf, insn);
                int sidx = ensure_sym(as, sym);
                objfile_add_reloc(as->obj, SEC_TEXT, off, sidx, rtype, 0);
                return true;
            }
            if (is_imm) {
                int64_t imm = IMM(2);
                int shift = 0;
                if (nops > 3 && strstr(ops[3], "lsl")) {
                    char *p = ops[3];
                    while (*p && !isdigit((unsigned char)*p)) p++;
                    shift = atoi(p) / 12; // 0 or 1 (for shift12)
                }
                uint32_t insn = is_sub ? (set_flags ? arm64_subs_imm(sf, r0, r1, (int32_t)imm, shift) : arm64_sub_imm(sf, r0, r1, (int32_t)imm, shift)) : (set_flags ? arm64_adds_imm(sf, r0, r1, (int32_t)imm, shift) : arm64_add_imm(sf, r0, r1, (int32_t)imm, shift));
                secbuf_emit32le(buf, insn);
            } else {
                uint32_t insn = is_sub ? (set_flags ? arm64_subs_reg(sf, r0, r1, r2, ARM64_LSL, 0) : arm64_sub_reg(sf, r0, r1, r2, ARM64_LSL, 0)) : (set_flags ? arm64_adds_reg(sf, r0, r1, r2, ARM64_LSL, 0) : arm64_add_reg(sf, r0, r1, r2, ARM64_LSL, 0));
                secbuf_emit32le(buf, insn);
            }
        }
        return true;
    }

    // CMP → SUBS xzr, rn, operand
    if (!strcmp(mnem, "cmp") || !strcmp(mnem, "cmn")) {
        bool is_cmn = !strcmp(mnem, "cmn");
        if (nops >= 2) {
            bool is_imm = (ops[1][0] == '#' || isdigit((unsigned char)ops[1][0]));
            if (is_imm) {
                int64_t imm = IMM(1);
                uint32_t insn = is_cmn ? arm64_adds_imm(sf, 31, r0, (int32_t)imm, 0) : arm64_subs_imm(sf, 31, r0, (int32_t)imm, 0);
                secbuf_emit32le(buf, insn);
            } else {
                uint32_t insn = is_cmn ? arm64_adds_reg(sf, 31, r0, r1, ARM64_LSL, 0) : arm64_subs_reg(sf, 31, r0, r1, ARM64_LSL, 0);
                secbuf_emit32le(buf, insn);
            }
        }
        return true;
    }

    // MUL, SDIV, UDIV, SMULL, UMULL, SMULH, UMULH
    if (!strcmp(mnem, "mul")) {
        secbuf_emit32le(buf, arm64_mul(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "sdiv")) {
        secbuf_emit32le(buf, arm64_sdiv(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "udiv")) {
        secbuf_emit32le(buf, arm64_udiv(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "smull")) {
        secbuf_emit32le(buf, arm64_smull(r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "umull")) {
        secbuf_emit32le(buf, arm64_umull(r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "smulh")) {
        secbuf_emit32le(buf, arm64_smulh(r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "umulh")) {
        secbuf_emit32le(buf, arm64_umulh(r0, r1, r2));
        return true;
    }

    // Logic register
    if (!strcmp(mnem, "and")) {
        secbuf_emit32le(buf, arm64_and_reg(sf, r0, r1, r2, ARM64_LSL, 0));
        return true;
    }
    if (!strcmp(mnem, "orr")) {
        secbuf_emit32le(buf, arm64_orr_reg(sf, r0, r1, r2, ARM64_LSL, 0));
        return true;
    }
    if (!strcmp(mnem, "eor")) {
        secbuf_emit32le(buf, arm64_eor_reg(sf, r0, r1, r2, ARM64_LSL, 0));
        return true;
    }
    if (!strcmp(mnem, "bic")) {
        secbuf_emit32le(buf, arm64_bic_reg(sf, r0, r1, r2, ARM64_LSL, 0));
        return true;
    }
    if (!strcmp(mnem, "ands")) {
        secbuf_emit32le(buf, arm64_ands_reg(sf, r0, r1, r2, ARM64_LSL, 0));
        return true;
    }
    // TST → ANDS xzr, rn, rm
    if (!strcmp(mnem, "tst")) {
        secbuf_emit32le(buf, arm64_ands_reg(sf, 31, r0, r1, ARM64_LSL, 0));
        return true;
    }

    // Shifts
    if (!strcmp(mnem, "lsl")) {
        bool imm = (nops > 2 && (ops[2][0] == '#' || isdigit((unsigned char)ops[2][0])));
        if (imm) secbuf_emit32le(buf, arm64_lsl_imm(sf, r0, r1, (int)IMM(2)));
        else
            secbuf_emit32le(buf, arm64_lsl_reg(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "lsr")) {
        bool imm = (nops > 2 && (ops[2][0] == '#' || isdigit((unsigned char)ops[2][0])));
        if (imm) secbuf_emit32le(buf, arm64_lsr_imm(sf, r0, r1, (int)IMM(2)));
        else
            secbuf_emit32le(buf, arm64_lsr_reg(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "asr")) {
        bool imm = (nops > 2 && (ops[2][0] == '#' || isdigit((unsigned char)ops[2][0])));
        if (imm) secbuf_emit32le(buf, arm64_asr_imm(sf, r0, r1, (int)IMM(2)));
        else
            secbuf_emit32le(buf, arm64_asr_reg(sf, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "ror")) {
        secbuf_emit32le(buf, arm64_ror_reg(sf, r0, r1, r2));
        return true;
    }

    // CLZ, CLS, RBIT, REV
    if (!strcmp(mnem, "clz")) {
        secbuf_emit32le(buf, arm64_clz(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "cls")) {
        secbuf_emit32le(buf, arm64_cls(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "rbit")) {
        secbuf_emit32le(buf, arm64_rbit(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "rev")) {
        secbuf_emit32le(buf, arm64_rev(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "rev16")) {
        secbuf_emit32le(buf, arm64_rev16(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "rev32")) {
        secbuf_emit32le(buf, arm64_rev32(r0, r1));
        return true;
    }

    // Extend
    if (!strcmp(mnem, "sxtb")) {
        secbuf_emit32le(buf, arm64_sxtb(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "sxth")) {
        secbuf_emit32le(buf, arm64_sxth(sf, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "sxtw")) {
        secbuf_emit32le(buf, arm64_sxtw(r0, r1));
        return true;
    }
    if (!strcmp(mnem, "uxtb")) {
        secbuf_emit32le(buf, arm64_uxtb(r0, r1));
        return true;
    }
    if (!strcmp(mnem, "uxth")) {
        secbuf_emit32le(buf, arm64_uxth(r0, r1));
        return true;
    }
    if (!strcmp(mnem, "ubfx")) {
        secbuf_emit32le(buf, arm64_ubfx(sf, r0, r1, (int)IMM(2), (int)IMM(3) + 1));
        return true;
    }

    // NEG / NEGS
    if (!strcmp(mnem, "neg") || !strcmp(mnem, "negs")) {
        secbuf_emit32le(buf, arm64_neg(sf, r0, r1));
        return true;
    }

    // Conditional select
    if (!strcmp(mnem, "csel")) {
        secbuf_emit32le(buf, arm64_csel(sf, r0, r1, r2, parse_arm64_cond(ops[3])));
        return true;
    }
    if (!strcmp(mnem, "csinc")) {
        secbuf_emit32le(buf, arm64_csinc(sf, r0, r1, r2, parse_arm64_cond(ops[3])));
        return true;
    }
    if (!strcmp(mnem, "csneg")) {
        secbuf_emit32le(buf, arm64_csneg(sf, r0, r1, r2, parse_arm64_cond(ops[3])));
        return true;
    }
    if (!strcmp(mnem, "cset")) {
        secbuf_emit32le(buf, arm64_cset(sf, r0, parse_arm64_cond(ops[1])));
        return true;
    }
    if (!strcmp(mnem, "cneg")) {
        secbuf_emit32le(buf, arm64_cneg(sf, r0, r1, parse_arm64_cond(ops[2])));
        return true;
    }

    // ADRP
    if (!strcmp(mnem, "adrp")) {
        char *sym = ops[1];
        uint32_t rtype = R_AARCH64_ADR_PREL_PG_HI21;
        const char *sname = parse_arm64_sym_reloc(sym, &rtype);
        if (rtype == 0) rtype = R_AARCH64_ADR_PREL_PG_HI21;
        size_t off = secbuf_emit32le(buf, arm64_adrp(r0, 0));
        int sidx = ensure_sym(as, sname);
        objfile_add_reloc(as->obj, SEC_TEXT, off, sidx, rtype, 0);
        return true;
    }
    if (!strcmp(mnem, "adr")) {
        size_t off = secbuf_emit32le(buf, arm64_adr(r0, 0));
        int sidx = ensure_sym(as, ops[1]);
        objfile_add_reloc(as->obj, SEC_TEXT, off, sidx, R_AARCH64_ADR_PREL_PG_HI21, 0);
        return true;
    }

    // Load/Store
    if (!strncmp(mnem, "ldr", 3) || !strncmp(mnem, "str", 3) ||
        !strncmp(mnem, "ldp", 3) || !strncmp(mnem, "stp", 3) ||
        !strncmp(mnem, "ldur", 4) || !strncmp(mnem, "stur", 4) ||
        !strncmp(mnem, "ldx", 3) || !strncmp(mnem, "stx", 3) ||
        !strncmp(mnem, "lda", 3) || !strncmp(mnem, "stl", 3)) {
        // Parse memory operand (last one for store, second for load)
        // LDR/LDUR/LDRB/LDRH rt, [rn, #imm]
        // STR/STUR/STRB/STRH rt, [rn, #imm]
        bool is_load = (mnem[0] == 'l');
        bool is_pair = (!strncmp(mnem, "ldp", 3) || !strncmp(mnem, "stp", 3));
        bool is_ldur = (!strncmp(mnem, "ldur", 4) || !strncmp(mnem, "stur", 4));
        bool is_byte = strstr(mnem, "b") != NULL && !strstr(mnem, "bl");
        bool is_half = strstr(mnem, "h") != NULL;
        bool is_sw = strstr(mnem, "sw") != NULL; // ldrsw
        bool is_exc = (mnem[0] == 'l' && mnem[1] == 'd' && mnem[2] == 'x') || (mnem[0] == 's' && mnem[1] == 't' && mnem[2] == 'x');
        bool is_acq = strstr(mnem, "lda") != NULL;
        bool is_rel = strstr(mnem, "stl") != NULL;

        if (is_pair) {
            // LDP/STP rt1, rt2, [rn, #imm]!  or  [rn], #imm
            int rt1 = r0, rt2 = r1;
            int64_t imm = 0;
            bool pre = false, post = false;
            int rn = parse_arm64_mem(ops[2], &imm, &pre, &post, NULL);
            int32_t imm7 = (int32_t)(imm / (sf ? 8 : 4));
            uint32_t insn = is_load ? arm64_ldp(sf, rt1, rt2, rn, imm7, pre, post) : arm64_stp(sf, rt1, rt2, rn, imm7, pre, post);
            secbuf_emit32le(buf, insn);
            return true;
        }

        int rt = r0;
        int memop_idx = is_load ? 1 : (is_pair ? 2 : 1);
        int64_t imm = 0;
        bool pre = false, post = false;
        int rn2 = -1;
        int rn = parse_arm64_mem(ops[memop_idx], &imm, &pre, &post, &rn2);

        if (is_exc) {
            if (is_load) {
                uint32_t insn = is_byte ? arm64_ldxrb(rt, rn) : is_half ? arm64_ldxrh(rt, rn)
                                                                        : arm64_ldxr(sf, rt, rn);
                secbuf_emit32le(buf, insn);
            } else {
                int rs = r1; // status register for stxr
                // stxr rs, rt, [rn]
                uint32_t insn = is_byte ? arm64_stxrb(rs, rt, rn) : is_half ? arm64_stxrh(rs, rt, rn)
                                                                            : arm64_stxr(sf, rs, rt, rn);
                secbuf_emit32le(buf, insn);
            }
            return true;
        }

        if (is_acq || is_rel) {
            if (is_load) {
                uint32_t insn = is_byte ? arm64_ldarb(rt, rn) : is_half ? arm64_ldarh(rt, rn)
                                                                        : arm64_ldar(sf, rt, rn);
                secbuf_emit32le(buf, insn);
            } else {
                uint32_t insn = is_byte ? arm64_stlrb(rt, rn) : is_half ? arm64_stlrh(rt, rn)
                                                                        : arm64_stlr(sf, rt, rn);
                secbuf_emit32le(buf, insn);
            }
            return true;
        }

        // Register offset?
        if (rn2 >= 0) {
            int sz = is_byte ? 0 : is_half ? 1
                : sf                       ? 3
                                           : 2;
            uint32_t insn = is_load ? arm64_ldr_reg(sz, rt, rn, rn2, false, 0) : arm64_str_reg(sz, rt, rn, rn2, false, 0);
            secbuf_emit32le(buf, insn);
            return true;
        }

        if (is_ldur) {
            uint32_t insn = is_load ? arm64_ldur(sf, rt, rn, (int32_t)imm) : arm64_stur(sf, rt, rn, (int32_t)imm);
            secbuf_emit32le(buf, insn);
            return true;
        }

        // Check for symbol reference (LDR rt, =sym or LDR rt, [rn, :got_lo12:sym])
        if (ops[memop_idx][0] == ':' || (ops[memop_idx][0] == '[' && strstr(ops[memop_idx], ":"))) {
            // Memory with relocation (already parsed rn above, but addend is reloc)
            char *rel_part = strstr(ops[memop_idx], ":");
            uint32_t rtype = 0;
            const char *sym = rel_part ? parse_arm64_sym_reloc(rel_part, &rtype) : NULL;
            int sz = is_byte ? 0 : is_half ? 1
                : sf                       ? 3
                                           : 2;
            uint32_t insn = is_load ? arm64_ldr_uoff(sz, rt, rn, 0) : arm64_str_uoff(sz, rt, rn, 0);
            size_t off = secbuf_emit32le(buf, insn);
            if (sym && rtype) {
                int sidx = ensure_sym(as, sym);
                objfile_add_reloc(as->obj, SEC_TEXT, off, sidx, rtype, 0);
            }
            return true;
        }

        // Standard immediate offset
        int32_t simm = (int32_t)imm;
        uint32_t insn;
        if (is_byte) {
            uint32_t uoff = simm >= 0 ? (uint32_t)simm : 0;
            insn = is_load ? (simm >= 0 ? arm64_ldrb_uoff(rt, rn, uoff) : arm64_ldrb_imm(rt, rn, simm)) : (simm >= 0 ? arm64_strb_uoff(rt, rn, uoff) : arm64_strb_imm(rt, rn, simm));
        } else if (is_half) {
            uint32_t uoff = simm >= 0 ? (uint32_t)(simm / 2) : 0;
            insn = is_load ? (simm >= 0 ? arm64_ldrh_uoff(rt, rn, uoff) : arm64_ldrh_imm(rt, rn, simm)) : (simm >= 0 ? arm64_strh_uoff(rt, rn, uoff) : arm64_strh_imm(rt, rn, simm));
        } else if (is_sw) {
            uint32_t uoff = simm >= 0 ? (uint32_t)(simm / 4) : 0;
            insn = simm >= 0 ? arm64_ldrsw_uoff(rt, rn, uoff) : arm64_ldrsw_imm(rt, rn, simm);
        } else {
            int sz = sf ? 3 : 2;
            uint32_t uoff = simm >= 0 ? (uint32_t)(simm / (sf ? 8 : 4)) : 0;
            if (pre || post) {
                insn = is_load ? arm64_ldr_imm(sf, rt, rn, simm, pre) : arm64_str_imm(sf, rt, rn, simm, pre);
            } else if (simm >= 0) {
                insn = is_load ? arm64_ldr_uoff(sz, rt, rn, uoff) : arm64_str_uoff(sz, rt, rn, uoff);
            } else {
                insn = is_load ? arm64_ldur(sf, rt, rn, simm) : arm64_stur(sf, rt, rn, simm);
            }
        }
        secbuf_emit32le(buf, insn);
        return true;
    }

    // DMB / DSB / ISB
    if (!strcmp(mnem, "dmb")) {
        secbuf_emit32le(buf, arm64_dmb(0xb));
        return true;
    }
    if (!strcmp(mnem, "dsb")) {
        secbuf_emit32le(buf, arm64_dsb(0xb));
        return true;
    }
    if (!strcmp(mnem, "isb")) {
        secbuf_emit32le(buf, arm64_isb());
        return true;
    }

    // PRFM
    if (!strcmp(mnem, "prfm")) {
        secbuf_emit32le(buf, arm64_nop());
        return true;
    } // simplification

    // FP: FMOV, FADD, FSUB, FMUL, FDIV, FCMP, FCVT, SCVTF, UCVTF, FCVTZS, FCVTZU, FNEG
    if (!strcmp(mnem, "fmov")) {
        if (nops == 2) {
            // Could be fmov Xd, Sn (FP→GP) or fmov Sd, Xn (GP→FP) or fmov Sd, Sn
            bool d_fp = (ops[0][0] == 'd' || ops[0][0] == 's');
            bool s_fp = (ops[1][0] == 'd' || ops[1][0] == 's');
            bool d_gp = (ops[0][0] == 'x' || ops[0][0] == 'w');
            bool s_gp = (ops[1][0] == 'x' || ops[1][0] == 'w');
            bool is_dbl = (ops[0][0] == 'd' || ops[1][0] == 'd' || ops[0][0] == 'x');
            int ftype = is_dbl ? 1 : 0;
            if (d_gp && s_fp)
                secbuf_emit32le(buf, arm64_fmov_f2i(is_dbl ? 1 : 0, r0, r1));
            else if (d_fp && s_gp)
                secbuf_emit32le(buf, arm64_fmov_i2f(is_dbl ? 1 : 0, r0, r1));
            else { // FP to FP
                // Use FMOV (register) encoding
                secbuf_emit32le(buf, 0x1e204000u | ((uint32_t)ftype << 22) | ((uint32_t)r1 << 5) | (uint32_t)r0);
            }
        }
        return true;
    }
    if (!strcmp(mnem, "fadd")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fadd(db ? 1 : 0, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "fsub")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fsub(db ? 1 : 0, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "fmul")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fmul(db ? 1 : 0, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "fdiv")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fdiv(db ? 1 : 0, r0, r1, r2));
        return true;
    }
    if (!strcmp(mnem, "fneg")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fneg(db ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "fcmp")) {
        bool db = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_fcmp(db ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "scvtf")) {
        bool src64 = (ops[1][0] == 'x');
        bool dstd = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_scvtf(src64 ? 1 : 0, dstd ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "ucvtf")) {
        bool src64 = (ops[1][0] == 'x');
        bool dstd = (ops[0][0] == 'd');
        secbuf_emit32le(buf, arm64_ucvtf(src64 ? 1 : 0, dstd ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "fcvtzs")) {
        bool dst64 = (ops[0][0] == 'x');
        bool srcd = (ops[1][0] == 'd');
        secbuf_emit32le(buf, arm64_fcvtzs(dst64 ? 1 : 0, srcd ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "fcvtzu")) {
        bool dst64 = (ops[0][0] == 'x');
        bool srcd = (ops[1][0] == 'd');
        secbuf_emit32le(buf, arm64_fcvtzu(dst64 ? 1 : 0, srcd ? 1 : 0, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "fcvt")) {
        // FCVT Dd, Sn or FCVT Sd, Dn
        bool dst_d = ops[0][0] == 'd';
        bool src_d = ops[1][0] == 'd';
        int ftype = src_d ? 1 : 0;
        int opc = dst_d ? 1 : 0;
        secbuf_emit32le(buf, arm64_fcvt(opc, ftype, r0, r1));
        return true;
    }
    if (!strcmp(mnem, "cnt")) {
        secbuf_emit32le(buf, arm64_nop());
        return true;
    } // vector cnt, simplify
    if (!strcmp(mnem, "addv")) {
        secbuf_emit32le(buf, arm64_nop());
        return true;
    } // vector addv
    if (!strcmp(mnem, "ins")) {
        secbuf_emit32le(buf, arm64_nop());
        return true;
    } // vector insert

    // Unknown — emit NOP as fallback and warn
    fprintf(stderr, "warning: unknown arm64 instruction: %s\n", mnem);
    secbuf_emit32le(buf, arm64_nop());
    return true;

#undef REG
#undef REG32
#undef IMM
#undef SF
}

// ---------------------------------------------------------------------------
// x86-64 instruction encoding dispatch
// ---------------------------------------------------------------------------

// Returns 0 if no explicit AT&T size suffix, else 1/2/4/8.
// "sub" ends with 'b' but that's the mnemonic, not a suffix.
// We maintain a list of known base names that don't carry size suffixes.
static int suffix_size(const char *mnem) {
    static const char *no_sfx[] = {
        "sub", "add", "and", "or", "xor", "not", "neg", "inc", "dec",
        "mul", "div", "imul", "idiv", "cmp", "test", "mov", "lea",
        "shl", "shr", "sal", "sar", "rol", "ror", "rcl", "rcr",
        "push", "pop", "call", "ret", "jmp", "nop", "xchg",
        "bsf", "bsr", "popcnt", "lzcnt", "tzcnt", "bswap",
        "movabs", "lock", "rep", "repe", "repne", "cld", "mfence",
        NULL};
    for (int i = 0; no_sfx[i]; i++)
        if (!strcmp(mnem, no_sfx[i])) return 0;
    int n = (int)strlen(mnem);
    if (n < 2) return 8;
    char last = mnem[n - 1];
    if (last == 'q') return 8;
    if (last == 'l') return 4;
    if (last == 'w') return 2;
    if (last == 'b') return 1;
    return 8;
}

// Operand helpers for x86 encoding — macros to avoid nested functions
#define X86_R(i)    ((i) < nops ? parse_x86_reg64(ops[i]) : X86_NOREG)
#define X86_IMM(i)  ((i) < nops ? (ops[i][0]=='$' ? strtoll(ops[i]+1,NULL,0) : strtoll(ops[i],NULL,0)) : (int64_t)0)
#define X86_ISREG(i) ((i)<nops && ops[i][0]=='%')
#define X86_ISIMM(i) ((i)<nops && ops[i][0]=='$')
#define X86_ISMEM(i) ((i)<nops && strchr(ops[i],'(')!=NULL)
#define X86_ISSYM(i) ((i)<nops && ops[i][0]!='%' && ops[i][0]!='$' && \
                      (isalpha((unsigned char)ops[i][0])||ops[i][0]=='_'||ops[i][0]=='.'||ops[i][0]=='-'))
static X86Mem x86_get_mem(char **ops, int nops, int i) {
    X86Mem m = {X86_NOREG, X86_NOREG, 1, 0};
    if (i < nops) parse_x86_mem(ops[i], &m);
    return m;
}
#define X86_M(i)    x86_get_mem(ops, nops, (i))

static bool encode_x86(AsmState *as, const char *mnem, char *ops_str) {
    char ops_buf[512];
    strncpy(ops_buf, ops_str, 511);
    ops_buf[511] = 0;
    char *ops[6];
    int nops = split_operands(ops_buf, ops, 6);
    SecBuf *buf = &as->obj->text;

    // Determine operand size from mnemonic suffix (0 = derive from operand)
    int sz = suffix_size(mnem);

// Shorten operand access
#define R(i)      X86_R(i)
#define IMM(i)    X86_IMM(i)
#define M(i)      X86_M(i)
#define is_reg(i) X86_ISREG(i)
#define is_imm(i) X86_ISIMM(i)
#define is_mem(i) X86_ISMEM(i)
#define is_sym(i) X86_ISSYM(i)

    // If no explicit suffix (sz==0), derive size from register operands
    if (sz == 0) {
        sz = 8; // default to 64-bit
        for (int _i = 0; _i < nops; _i++) {
            if (X86_ISREG(_i)) {
                int rsz = reg_size_x86(ops[_i]);
                if (rsz != 8) {
                    sz = rsz;
                    break;
                } // prefer explicit sub-64-bit size
                sz = rsz; // keep 8 but may be overridden
                break;
            }
        }
    }

    if (!strcmp(mnem, "nop")) {
        x86_nop(buf);
        return true;
    }
    if (!strcmp(mnem, "ret") || !strcmp(mnem, "retq")) {
        x86_ret(buf);
        return true;
    }
    if (!strcmp(mnem, "leave") || !strcmp(mnem, "leaveq")) {
        x86_leave(buf);
        return true;
    }
    if (!strcmp(mnem, "cld")) {
        x86_cld(buf);
        return true;
    }
    if (!strcmp(mnem, "mfence")) {
        x86_mfence(buf);
        return true;
    }
    if (!strcmp(mnem, "cdq")) {
        x86_cdq(buf);
        return true;
    }
    if (!strcmp(mnem, "cqo")) {
        x86_cqo(buf);
        return true;
    }

    // PUSH/POP
    if (!strncmp(mnem, "push", 4)) {
        if (is_imm(0)) x86_push_imm(buf, (int32_t)IMM(0));
        else
            x86_push(buf, R(0));
        return true;
    }
    if (!strncmp(mnem, "pop", 3)) {
        x86_pop(buf, R(0));
        return true;
    }

    // MOV variants
    if (!strncmp(mnem, "mov", 3)) {
        // AT&T: src, dst
        bool is_movabs = strstr(mnem, "abs") != NULL;
        bool is_movsx = strstr(mnem, "sx") != NULL || strstr(mnem, "sl") != NULL;
        bool is_movzx = strstr(mnem, "zb") != NULL || strstr(mnem, "zw") != NULL || strstr(mnem, "zl") != NULL;
        bool is_movs = !is_movsx && !is_movzx && (strstr(mnem, "sbl") || strstr(mnem, "sbq") || strstr(mnem, "swl") || strstr(mnem, "swq") || strstr(mnem, "slq"));

        int src_sz = sz, dst_sz = sz;
        if (strstr(mnem, "bl") || strstr(mnem, "bq")) src_sz = 1;
        else if (strstr(mnem, "wl") || strstr(mnem, "wq"))
            src_sz = 2;
        else if (strstr(mnem, "lq"))
            src_sz = 4;

        if (is_movabs) {
            x86_movabs(buf, R(1), (uint64_t)IMM(0));
            return true;
        }
        if (is_movs || is_movsx) {
            // MOVSBL, MOVSBQ, etc.
            if (is_reg(0) && is_reg(1)) x86_movsx(buf, dst_sz, src_sz, R(1), R(0));
            else if (is_mem(0))
                x86_movsx_rm(buf, dst_sz, src_sz, R(1), M(0));
            return true;
        }
        if (is_movzx) {
            if (is_reg(0) && is_reg(1)) x86_movzx(buf, dst_sz, src_sz, R(1), R(0));
            else if (is_mem(0))
                x86_movzx_rm(buf, dst_sz, src_sz, R(1), M(0));
            return true;
        }
        // Regular MOV: src, dst (AT&T order)
        // AT&T: movq src, dst
        if (is_imm(0) && is_reg(1)) {
            x86_mov_ri(buf, sz, R(1), IMM(0));
            return true;
        }
        if (is_imm(0) && is_mem(1)) {
            x86_mov_mi(buf, sz, M(1), (int32_t)IMM(0));
            return true;
        }
        if (is_reg(0) && is_reg(1)) {
            x86_mov_rr(buf, sz, R(1), R(0));
            return true;
        }
        if (is_reg(0) && is_mem(1)) {
            x86_mov_mr(buf, sz, M(1), R(0));
            return true;
        }
        if (is_mem(0) && is_reg(1)) {
            const char *rip_sym;
            if (is_rip_rel(ops[0], &rip_sym))
                emit_rip_rel_load(as, rip_sym, x86_mov_rm, sz, R(1));
            else
                x86_mov_rm(buf, sz, R(1), M(0));
            return true;
        }
        if (is_reg(0) && is_sym(1)) {
            // movq %reg, sym (absolute address)
            // For now, treat as reg→mem with symbol offset (not fully correct for PIC)
        }
        return true;
    }

    // LEA
    if (!strncmp(mnem, "lea", 3)) {
        if (is_mem(0) && is_reg(1)) {
            const char *rip_sym;
            if (is_rip_rel(ops[0], &rip_sym))
                emit_rip_rel_load(as, rip_sym, x86_lea, sz, R(1));
            else
                x86_lea(buf, sz, R(1), M(0));
        }
        return true;
    }

// ADD / SUB / AND / OR / XOR  (all four operand combinations)
#define ALU_OP(name, fn_rr, fn_ri, fn_rm, fn_mi) \
    if (!strncmp(mnem, name, strlen(name))) { \
        if (is_imm(0)&&is_reg(1))  { fn_ri(buf,sz,R(1),(int32_t)IMM(0)); } \
        else if (is_reg(0)&&is_reg(1))  { fn_rr(buf,sz,R(1),R(0)); } \
        else if (is_mem(0)&&is_reg(1))  { fn_rm(buf,sz,R(1),M(0)); } \
        else if (is_imm(0)&&is_mem(1))  { fn_mi(buf,sz,M(1),(int32_t)IMM(0)); } \
        else if (is_reg(0)&&is_mem(1))  { fn_mi(buf,sz,M(1),0); /* fallback: treat reg as imm0 */ } \
        return true; \
    }
    ALU_OP("add", x86_add_rr, x86_add_ri, x86_add_rm, x86_add_mi)
    ALU_OP("sub", x86_sub_rr, x86_sub_ri, x86_sub_rm, x86_sub_mi)
    ALU_OP("and", x86_and_rr, x86_and_ri, x86_and_rm, x86_and_mi)
    ALU_OP("or", x86_or_rr, x86_or_ri, x86_add_rm, x86_or_mi)
    ALU_OP("xor", x86_xor_rr, x86_xor_ri, x86_xor_rm, x86_xor_mi)
#undef ALU_OP

    // IMUL
    if (!strncmp(mnem, "imul", 4)) {
        if (nops == 2 && is_reg(0) && is_reg(1)) x86_imul_rr(buf, sz, R(1), R(0));
        else if (nops == 3 && is_imm(2))
            x86_imul_rri(buf, sz, R(1), R(0), (int32_t)IMM(2));
        else if (nops == 1)
            x86_imul_r(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "idiv", 4)) {
        x86_idiv_r(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "div", 3)) {
        x86_div_r(buf, sz, R(0));
        return true;
    }

    // NEG / NOT
    if (!strncmp(mnem, "neg", 3)) {
        x86_neg_r(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "not", 3)) {
        x86_not_r(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "inc", 3)) {
        x86_inc_r(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "dec", 3)) {
        x86_dec_r(buf, sz, R(0));
        return true;
    }

    // SHIFTS (AT&T: count, r/m)
    if (!strncmp(mnem, "shl", 3) || !strncmp(mnem, "sal", 3)) {
        if (is_imm(0)) x86_shl_ri(buf, sz, R(1), (uint8_t)IMM(0));
        else
            x86_shl_rcl(buf, sz, R(1));
        return true;
    }
    if (!strncmp(mnem, "shr", 3)) {
        if (is_imm(0)) x86_shr_ri(buf, sz, R(1), (uint8_t)IMM(0));
        else
            x86_shr_rcl(buf, sz, R(1));
        return true;
    }
    if (!strncmp(mnem, "sar", 3)) {
        if (is_imm(0))
            x86_sar_ri(buf, sz, R(1), (uint8_t)IMM(0));
        else
            x86_sar_rcl(buf, sz, R(1));
        return true;
    }
    if (!strncmp(mnem, "ror", 3)) {
        x86_ror_ri(buf, sz, R(1), (uint8_t)IMM(0));
        return true;
    }
    if (!strncmp(mnem, "rol", 3)) {
        x86_rol_ri(buf, sz, R(1), (uint8_t)IMM(0));
        return true;
    }

    // CMP / TEST
    if (!strncmp(mnem, "cmp", 3)) {
        if (is_imm(0) && is_reg(1)) x86_cmp_ri(buf, sz, R(1), (int32_t)IMM(0));
        else if (is_reg(0) && is_reg(1))
            x86_cmp_rr(buf, sz, R(1), R(0));
        else if (is_mem(0) && is_reg(1))
            x86_cmp_rm(buf, sz, R(1), M(0));
        else if (is_reg(0) && is_mem(1))
            x86_cmp_mr(buf, sz, M(1), R(0));
        else if (is_imm(0) && is_mem(1))
            x86_cmp_mi(buf, sz, M(1), (int32_t)IMM(0));
        return true;
    }
    if (!strncmp(mnem, "test", 4)) {
        if (is_imm(0) && is_reg(1)) x86_test_ri(buf, sz, R(1), (int32_t)IMM(0));
        else if (is_reg(0) && is_reg(1))
            x86_test_rr(buf, sz, R(1), R(0));
        return true;
    }

    // SETcc
    if (!strncmp(mnem, "set", 3)) {
        const char *cc_s = mnem + 3;
        X86Cond cc = X86_E;
        if (!strcmp(cc_s, "e") || !strcmp(cc_s, "z")) cc = X86_E;
        else if (!strcmp(cc_s, "ne") || !strcmp(cc_s, "nz"))
            cc = X86_NE;
        else if (!strcmp(cc_s, "l") || !strcmp(cc_s, "nge"))
            cc = X86_L;
        else if (!strcmp(cc_s, "le") || !strcmp(cc_s, "ng"))
            cc = X86_LE;
        else if (!strcmp(cc_s, "g") || !strcmp(cc_s, "nle"))
            cc = X86_G;
        else if (!strcmp(cc_s, "ge") || !strcmp(cc_s, "nl"))
            cc = X86_GE;
        else if (!strcmp(cc_s, "b") || !strcmp(cc_s, "c") || !strcmp(cc_s, "nae"))
            cc = X86_B;
        else if (!strcmp(cc_s, "be") || !strcmp(cc_s, "na"))
            cc = X86_BE;
        else if (!strcmp(cc_s, "a") || !strcmp(cc_s, "nbe"))
            cc = X86_A;
        else if (!strcmp(cc_s, "ae") || !strcmp(cc_s, "nc"))
            cc = X86_AE;
        else if (!strcmp(cc_s, "s"))
            cc = X86_S;
        else if (!strcmp(cc_s, "ns"))
            cc = X86_NS;
        else if (!strcmp(cc_s, "p"))
            cc = X86_P;
        else if (!strcmp(cc_s, "np") || !strcmp(cc_s, "po"))
            cc = X86_NP;
        x86_setcc(buf, cc, R(0));
        return true;
    }

    // CMOVcc
    if (!strncmp(mnem, "cmov", 4)) {
        const char *cc_s = mnem + 4;
        X86Cond cc = X86_NE;
        if (!strcmp(cc_s, "nz") || !strcmp(cc_s, "ne")) cc = X86_NE;
        else if (!strcmp(cc_s, "z") || !strcmp(cc_s, "e"))
            cc = X86_E;
        // others...
        if (is_reg(0) && is_reg(1)) x86_cmovcc(buf, sz, cc, R(1), R(0));
        return true;
    }

    // JMP and Jcc
    if (!strcmp(mnem, "jmp") || !strcmp(mnem, "jmpq")) {
        // Strip AT&T indirect prefix '*' (e.g. jmp *%r10 → jmp %r10)
        if (nops > 0 && ops[0][0] == '*') ops[0]++;
        if (is_reg(0)) {
            x86_jmp_r(buf, R(0));
            return true;
        }
        // Label-based jump
        char *lbl = ops[0];
        size_t off = buf->len;
        x86_jmp_rel32(buf, 0); // placeholder
        int sec = 0;
        int64_t toff = lookup_local(as, lbl, &sec);
        if (toff >= 0 && sec == SEC_TEXT) {
            int32_t delta = (int32_t)(toff - (int64_t)(off + 5));
            secbuf_patch32le(buf, off + 1, (uint32_t)delta);
        } else {
            add_fixup(as, off + 1, SEC_TEXT, lbl, FIXUP_REL32, 0);
        }
        return true;
    }

    // Jcc
    if (mnem[0] == 'j' && strlen(mnem) <= 6) {
        const char *cc_s = mnem + 1;
        X86Cond cc = X86_E;
        if (!strcmp(cc_s, "e") || !strcmp(cc_s, "z")) cc = X86_E;
        else if (!strcmp(cc_s, "ne") || !strcmp(cc_s, "nz"))
            cc = X86_NE;
        else if (!strcmp(cc_s, "l") || !strcmp(cc_s, "nge"))
            cc = X86_L;
        else if (!strcmp(cc_s, "le") || !strcmp(cc_s, "ng"))
            cc = X86_LE;
        else if (!strcmp(cc_s, "g") || !strcmp(cc_s, "nle"))
            cc = X86_G;
        else if (!strcmp(cc_s, "ge") || !strcmp(cc_s, "nl"))
            cc = X86_GE;
        else if (!strcmp(cc_s, "b") || !strcmp(cc_s, "c") || !strcmp(cc_s, "nae"))
            cc = X86_B;
        else if (!strcmp(cc_s, "be") || !strcmp(cc_s, "na"))
            cc = X86_BE;
        else if (!strcmp(cc_s, "a") || !strcmp(cc_s, "nbe"))
            cc = X86_A;
        else if (!strcmp(cc_s, "ae") || !strcmp(cc_s, "nc"))
            cc = X86_AE;
        else if (!strcmp(cc_s, "s"))
            cc = X86_S;
        else if (!strcmp(cc_s, "ns"))
            cc = X86_NS;
        else if (!strcmp(cc_s, "p"))
            cc = X86_P;
        else if (!strcmp(cc_s, "np") || !strcmp(cc_s, "po"))
            cc = X86_NP;
        else if (!strcmp(cc_s, "mp")) { // jmp → already handled
            x86_jmp_rel32(buf, 0);
            return true;
        }
        char *lbl = ops[0];
        size_t off = buf->len;
        x86_jcc_rel32(buf, cc, 0); // emits 0F 8X imm32 (6 bytes total)
        int sec = 0;
        int64_t toff = lookup_local(as, lbl, &sec);
        if (toff >= 0 && sec == SEC_TEXT) {
            int32_t delta = (int32_t)(toff - (int64_t)(off + 6));
            secbuf_patch32le(buf, off + 2, (uint32_t)delta);
        } else {
            add_fixup(as, off + 2, SEC_TEXT, lbl, FIXUP_REL32, 0);
        }
        return true;
    }

    // CALL
    if (!strcmp(mnem, "call") || !strcmp(mnem, "callq")) {
        // Strip AT&T indirect prefix '*' (e.g. call *%r10 → call %r10)
        if (nops > 0 && ops[0][0] == '*') ops[0]++;
        if (is_reg(0)) {
            x86_call_r(buf, R(0));
            return true;
        }
        char *lbl = ops[0];
        size_t off = buf->len;
        x86_call_rel32(buf, 0);
        int sidx = ensure_sym(as, lbl);
        // Check if local label
        int sec = 0;
        int64_t toff = lookup_local(as, lbl, &sec);
        if (toff >= 0 && sec == SEC_TEXT) {
            int32_t delta = (int32_t)(toff - (int64_t)(off + 5));
            secbuf_patch32le(buf, off + 1, (uint32_t)delta);
        } else {
            // External function → PLT32 reloc
            objfile_add_reloc(as->obj, SEC_TEXT, off + 1, sidx,
                              R_X86_64_PLT32, -4);
        }
        return true;
    }

    // BSF/BSR/POPCNT/LZCNT/TZCNT
    if (!strncmp(mnem, "bsf", 3)) {
        x86_bsf(buf, sz, R(1), R(0));
        return true;
    }
    if (!strncmp(mnem, "bsr", 3)) {
        x86_bsr(buf, sz, R(1), R(0));
        return true;
    }
    if (!strncmp(mnem, "popcnt", 6)) {
        x86_popcnt(buf, sz, R(1), R(0));
        return true;
    }
    if (!strncmp(mnem, "lzcnt", 5)) {
        x86_lzcnt(buf, sz, R(1), R(0));
        return true;
    }
    if (!strncmp(mnem, "tzcnt", 5)) {
        x86_tzcnt(buf, sz, R(1), R(0));
        return true;
    }
    if (!strncmp(mnem, "bswap", 5)) {
        x86_bswap(buf, sz, R(0));
        return true;
    }
    if (!strncmp(mnem, "xchg", 4)) {
        x86_xchg_rr(buf, sz, R(0), R(1));
        return true;
    }

    // Prefixes
    if (!strcmp(mnem, "lock")) {
        x86_lock_prefix(buf);
        return true;
    }
    if (!strcmp(mnem, "rep")) {
        x86_rep_prefix(buf);
        return true;
    }
    if (!strcmp(mnem, "repe")) {
        x86_rep_prefix(buf);
        return true;
    }
    if (!strcmp(mnem, "repne")) {
        x86_repne_prefix(buf);
        return true;
    }

    // SSE
    if (!strcmp(mnem, "movsd")) {
        if (is_reg(0) && is_reg(1))
            x86_movsd_rr(buf, (X86XmmReg)parse_x86_reg64(ops[0]), (X86XmmReg)parse_x86_reg64(ops[1]));
        else if (is_mem(0) && is_reg(1)) {
            const char *rip_sym;
            X86XmmReg xd = (X86XmmReg)R(1);
            if (is_rip_rel(ops[0], &rip_sym)) {
                X86Mem rip_m = {X86_NOREG, X86_NOREG, 1, 0};
                x86_movsd_rm(buf, xd, rip_m);
                int sidx = ensure_sym(as, rip_sym);
                objfile_add_reloc(as->obj, SEC_TEXT, buf->len - 4, sidx, R_X86_64_PC32, -4);
            } else {
                x86_movsd_rm(buf, xd, M(0));
            }
        } else if (is_reg(0) && is_mem(1))
            x86_movsd_mr(buf, M(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "movss")) {
        if (is_reg(0) && is_reg(1))
            x86_movss_rr(buf, (X86XmmReg)R(0), (X86XmmReg)R(1));
        else if (is_mem(0) && is_reg(1)) {
            const char *rip_sym;
            X86XmmReg xd = (X86XmmReg)R(1);
            if (is_rip_rel(ops[0], &rip_sym)) {
                X86Mem rip_m = {X86_NOREG, X86_NOREG, 1, 0};
                x86_movss_rm(buf, xd, rip_m);
                int sidx = ensure_sym(as, rip_sym);
                objfile_add_reloc(as->obj, SEC_TEXT, buf->len - 4, sidx, R_X86_64_PC32, -4);
            } else {
                x86_movss_rm(buf, xd, M(0));
            }
        }
        return true;
    }
    if (!strcmp(mnem, "addsd")) {
        x86_addsd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "subsd")) {
        x86_subsd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "mulsd")) {
        x86_mulsd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "divsd")) {
        x86_divsd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "addss")) {
        x86_addss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "subss")) {
        x86_subss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "mulss")) {
        x86_mulss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "divss")) {
        x86_divss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "ucomisd")) {
        x86_ucomisd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "ucomiss")) {
        x86_ucomiss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "comisd")) {
        x86_comisd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "cvtsi2sd")) {
        int ss = reg_size_x86(ops[0]);
        x86_cvtsi2sd(buf, ss, (X86XmmReg)R(1), R(0));
        return true;
    }
    if (!strcmp(mnem, "cvtsi2ss")) {
        int ss = reg_size_x86(ops[0]);
        x86_cvtsi2ss(buf, ss, (X86XmmReg)R(1), R(0));
        return true;
    }
    if (!strcmp(mnem, "cvttsd2si")) {
        int ds = reg_size_x86(ops[1]);
        x86_cvttsd2si(buf, ds, R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "cvttss2si")) {
        int ds = reg_size_x86(ops[1]);
        x86_cvttss2si(buf, ds, R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "cvtsd2ss")) {
        x86_cvtsd2ss(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "cvtss2sd")) {
        x86_cvtss2sd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "xorpd")) {
        x86_xorpd(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "xorps")) {
        x86_xorps(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "movaps")) {
        x86_movaps(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "pxor")) {
        x86_pxor(buf, (X86XmmReg)R(1), (X86XmmReg)R(0));
        return true;
    }
    if (!strcmp(mnem, "fldl")) {
        x86_fldl_m(buf, M(0));
        return true;
    }
    if (!strcmp(mnem, "fstpt")) {
        x86_fstpt_m(buf, M(0));
        return true;
    }

    // Unknown
    fprintf(stderr, "warning: unknown x86 instruction: %s\n", mnem);
#undef R
#undef IMM
#undef M
#undef is_reg
#undef is_imm
#undef is_mem
#undef is_sym
    return true;
}

// ---------------------------------------------------------------------------
// Main assembler loop
// ---------------------------------------------------------------------------
int assemble_file(const char *asm_path, const char *obj_path) {
    FILE *f = fopen(asm_path, "r");
    if (!f) {
        fprintf(stderr, "asm: cannot open %s\n", asm_path);
        return -1;
    }

    ObjFile obj;
    objfile_init(&obj);

    AsmState as = {0};
    as.obj = &obj;
    as.cur_sec = SEC_TEXT;
    as.filename = asm_path;

#ifdef ARCH_ARM64
    bool is_arm64 = true;
#else
    bool is_arm64 = false;
#endif

    char line[1024];
    // Pending "global function" declaration for next label
    bool pending_globl = false;
    bool pending_weak = false;
    bool pending_func = false;
    (void)pending_globl;
    (void)pending_weak;
    (void)pending_func;

    while (fgets(line, sizeof(line), f)) {
        as.lineno++;
        char *p = skip_ws(line);
        trim_end(p);

        // Skip empty lines and comments
        if (!*p || *p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/')) continue;
        // AT&T comment
        if (*p == '/') continue;

        // Labels (including ".L..." local labels) end with ":" at end-of-token.
        // Check for label BEFORE directive: ".L.return.main:" is a label, ".text" is a directive.
        char *colon = strchr(p, ':');
        bool is_label = colon && (colon[1] == '\0' || colon[1] == '\n' || colon[1] == ' ' || colon[1] == '\t');

        if (!is_label && *p == '.') {
            // Pure directive (no colon)
            char *dir = p + 1;
            char *sp = dir;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            char *args = *sp ? sp + 1 : sp;
            *sp = 0;
            for (char *d = dir; *d; d++) *d = tolower((unsigned char)*d);
            handle_directive(&as, dir, args);
            continue;
        }

        if (is_label) {
            *colon = 0;
            char *lbl = p;
            int idx = objfile_find_sym(&obj, lbl);
            bool is_global = (idx >= 0 && obj.syms[idx].bind != SB_LOCAL);
            bool is_weak = (idx >= 0 && obj.syms[idx].bind == SB_WEAK);
            bool is_func = (idx >= 0 && obj.syms[idx].type == ST_FUNC);
            define_label(&as, lbl, is_global, is_weak, is_func);
            p = skip_ws(colon + 1);
            if (!*p) continue;
        }

        if (as.cur_sec != SEC_TEXT) {
            // Non-text sections: only directives and labels handled above
            continue;
        }

        // Instruction: split mnemonic from operands
        char insn_buf[512];
        strncpy(insn_buf, p, 511);
        insn_buf[511] = 0;
        char *mnem = insn_buf;
        char *ops_str = mnem;
        while (*ops_str && !isspace((unsigned char)*ops_str)) ops_str++;
        if (*ops_str) {
            *ops_str++ = 0;
            ops_str = skip_ws(ops_str);
        }

        // Lowercase mnemonic
        for (char *m = mnem; *m; m++) *m = tolower((unsigned char)*m);

        // Remove trailing colon from mnemonic (shouldn't happen but defensive)
        char *mc = strchr(mnem, ':');
        if (mc) *mc = 0;

        // Strip comment from ops
        char *cmt = strstr(ops_str, " //");
        if (!cmt) cmt = strstr(ops_str, " #");
        if (cmt) *cmt = 0;

        if (!*mnem) continue;

        if (is_arm64)
            encode_arm64(&as, mnem, ops_str);
        else
            encode_x86(&as, mnem, ops_str);
    }

    fclose(f);

    // Check for unresolved fixups
    if (as.nfixups > 0) {
        fprintf(stderr, "asm: warning: %d unresolved fixups\n", as.nfixups);
    }

    // Write object file
    int rc;
#ifdef __APPLE__
    rc = macho_write(&obj, obj_path);
#else
    rc = elf_write(&obj, obj_path);
#endif

    objfile_free(&obj);
    return rc;
}
