// SPDX-License-Identifier: LGPL-2.1-or-later
// Derived from chibicc by Rui Ueyama.
#include "rcc.h"

typedef struct VarAttr VarAttr;
typedef struct TagScope TagScope;
typedef struct EnumConst EnumConst;

struct VarAttr {
    bool is_typedef;
    bool is_extern;
    bool is_static;
    bool is_inline;
    bool is_weak;
    bool has_type;
    unsigned char bitfield_mode;
};

struct TagScope {
    TagScope *next;
    char *name;
    Type *ty;
};

struct EnumConst {
    EnumConst *next;
    char *name;
    int64_t val;
};

static LVar *locals;
static LVar *globals;
static Typedef *typedefs;
static TagScope *tags;
static EnumConst *enum_consts;
static int stack_offset;
static char *pending_cleanup_func;
static Token *pending_cleanup_tok;
static bool pending_constructor;
static bool pending_destructor;
static char *pending_asm_name;
static char *pending_alias_target;

static StrLit *str_lits;
static int str_lit_counter;

static Node *current_switch;
static Node *current_loop;
static int static_local_counter;
static LVar *current_fn_scope_locals;
static char *parser_current_fn;
static int current_block_depth;
static bool suppress_fn_scope_update;
static bool fn_uses_vla;

typedef struct LabelScope LabelScope;
typedef struct PendingGoto PendingGoto;
struct LabelScope {
    LabelScope *next;
    char *name;
    LVar *locals;
};

static LabelScope *label_scopes;
struct PendingGoto {
    PendingGoto *next;
    char *name;
    Node *node;
};

static PendingGoto *pending_gotos;
static Node *conditional(Token **rest, Token *tok);

static bool equal(Token *tok, char *op) {
    if (!tok || !tok->loc) return false;
    int len = (int)strlen(op);
    return tok->len == len && memcmp(tok->loc, op, len) == 0;
}

// Fast variant for string-literal comparisons (avoids strlen at runtime)
#define equalc(tok, op)     ((tok) && (tok)->loc && (tok)->len == (int)(sizeof(op) - 1) &&      memcmp((tok)->loc, op, sizeof(op) - 1) == 0)

// Peek past a single __attribute__((...)) block without consuming tokens.
// Returns the token after the closing )), or NULL if structure doesn't match.
static Token *peek_past_attr(Token *tok) {
    if (!equalc(tok, "__attribute__") && !equalc(tok, "__attribute"))
        return NULL;
    tok = tok->next;
    if (!equalc(tok, "(")) return NULL;
    tok = tok->next;
    if (!equalc(tok, "(")) return NULL;
    tok = tok->next;
    int depth = 1;
    while (depth > 0 && tok->kind != TK_EOF) {
        if (equalc(tok, "(")) depth++;
        else if (equalc(tok, ")"))
            depth--;
        tok = tok->next;
    }
    if (!equalc(tok, ")")) return NULL; // final closing paren
    return tok->next;
}

static Token *skip(Token *tok, char *op) {
    if (!equal(tok, op))
        error_tok(tok, "expected specific operator");
    return tok->next;
}

static int align_to(int n, int align) {
    return (n + align - 1) & ~(align - 1);
}

static Node *new_node(NodeKind kind, Token *tok) {
    Node *node = arena_alloc(sizeof(Node));
    node->kind = kind;
    node->tok = tok;
    return node;
}

static Node *new_binary(NodeKind kind, Node *lhs, Node *rhs, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = lhs;
    node->rhs = rhs;
    return node;
}

static Node *new_unary(NodeKind kind, Node *expr, Token *tok) {
    Node *node = new_node(kind, tok);
    node->lhs = expr;
    return node;
}

// Insert implicit casts for function call arguments to match parameter types
static void cast_funcall_args(Node *call) {
    Type *fty = NULL;
    if (call->lhs) {
        add_type(call->lhs);
        Type *t = call->lhs->ty;
        if (t->kind == TY_PTR && t->base && t->base->kind == TY_FUNC)
            fty = t->base;
    }
    if (!fty || !fty->param_types)
        return;
    Type *pt = fty->param_types;
    for (Node **arg = &call->args; *arg && pt; arg = &(*arg)->next, pt = pt->param_next) {
        add_type(*arg);
        if (!(*arg)->ty)
            continue;
        bool arg_float = is_flonum((*arg)->ty);
        bool param_float = is_flonum(pt);
        bool arg_int = is_integer((*arg)->ty);
        bool param_int = is_integer(pt);
        if ((arg_int && param_float) || (arg_float && param_int) ||
            (arg_int && param_int && (*arg)->ty->size != pt->size)) {
            Node *cast = new_unary(ND_CAST, *arg, (*arg)->tok);
            cast->ty = arena_alloc(sizeof(Type));
            *cast->ty = *pt;
            cast->next = (*arg)->next;
            *arg = cast;
        }
    }
}

static Node *new_num(int64_t val, Token *tok) {
    Node *node = new_node(ND_NUM, tok);
    node->val = val;
    // Determine type from suffix encoded in token text
    char *end = tok->loc + tok->len;
    bool is_u = false;
    int l_count = 0;
    for (char *s = end - 1; s >= tok->loc; s--) {
        char c = *s;
        if (c == 'u' || c == 'U') {
            is_u = true;
        } else if (c == 'l' || c == 'L') {
            l_count++;
        } else
            break;
    }
    if (l_count >= 2)
        node->ty = is_u ? ty_ullong : ty_llong;
    else if (l_count == 1)
        node->ty = is_u ? ty_ulong : ty_long;
    else if (is_u)
        node->ty = (val >= 0 && val <= 0xFFFFFFFFLL) ? ty_uint : ty_ullong;
    else if (val >= -2147483648LL && val <= 2147483647LL)
        node->ty = ty_int;
    else if (tok->loc[0] == '0' && tok->len > 1 && val >= 0 && val <= 4294967295LL)
        node->ty = ty_uint;
    else
        node->ty = ty_llong;
    return node;
}

static Node *new_fnum(double fval, Token *tok) {
    Node *node = new_node(ND_FNUM, tok);
    node->fval = fval;
    node->ty = tok->val == 2 ? ty_ldouble : ty_double;
    return node;
}

// Compute the byte size expression for a VLA allocation: count * element_size
static Node *vla_alloc_size(Type *ty, Token *tok) {
    Node *base_sz = (ty->base->kind == TY_VLA)
        ? vla_alloc_size(ty->base, tok)
        : new_node(ND_NUM, tok);
    if (ty->base->kind != TY_VLA) {
        base_sz->val = ty->base->size;
        base_sz->ty = ty_ulong;
    }
    Node *count = ty->vla_len_expr ? ty->vla_len_expr : new_node(ND_NUM, tok);
    if (!ty->vla_len_expr) {
        count->val = ty->array_len;
        count->ty = ty_ulong;
    }
    return new_binary(ND_MUL, count, base_sz, tok);
}

static Type *copy_type(Type *ty) {
    // For struct/union types, return the original. These are identity types
    // that can be completed later via tag declarations. Creating a shallow
    // copy would leave the copy incomplete forever.
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION)
        return ty;
    Type *ret = arena_alloc(sizeof(Type));
    *ret = *ty;
    return ret;
}

static Type *apply_type_align(Type *ty, int align) {
    if (align <= 0 || align <= ty->align)
        return ty;
    Type *ret = copy_type(ty);
    ret->align = align;
    return ret;
}

static Type *func_type(Type *return_ty) {
    Type *ty = arena_alloc(sizeof(Type));
    ty->kind = TY_FUNC;
    ty->size = 1;
    ty->align = 1;
    ty->return_ty = return_ty;
    return ty;
}

static LVar *new_var(char *name, Type *ty, bool is_local) {
    LVar *var = arena_alloc(sizeof(LVar));
    var->name = name;
    var->ty = ty;
    var->is_local = is_local;
    var->alias_target = NULL;
    var->asm_name = NULL;

    if (is_local) {
        int size = ty->size < 4 ? 4 : ty->size;
        int align = ty->align < 4 ? 4 : ty->align;
        stack_offset = align_to(stack_offset + size, align);
        var->offset = stack_offset;
        var->next = locals;
        locals = var;
    } else {
        var->next = globals;
        globals = var;
    }
    return var;
}

static Node *new_var_node(LVar *var, Token *tok) {
    Node *node = new_node(ND_LVAR, tok);
    node->var = var;
    node->ty = var->ty;
    return node;
}

static LVar *find_var(Token *tok) {
    for (LVar *var = locals; var; var = var->next)
        if (var->name == tok->name)
            return var;
    for (LVar *var = globals; var; var = var->next)
        if (var->name == tok->name)
            return var;
    return NULL;
}

static LVar *find_global_name(char *name) {
    for (LVar *var = globals; var; var = var->next)
        if (var->name == name)
            return var;
    return NULL;
}

static LabelScope *find_label_scope(char *name) {
    for (LabelScope *ls = label_scopes; ls; ls = ls->next)
        if (ls->name == name)
            return ls;
    return NULL;
}

static void record_label_scope(char *name, LVar *locals_at_label) {
    LabelScope *ls = find_label_scope(name);
    if (ls) {
        ls->locals = locals_at_label;
        return;
    }

    ls = arena_alloc(sizeof(LabelScope));
    ls->name = name;
    ls->locals = locals_at_label;
    ls->next = label_scopes;
    label_scopes = ls;
}

static void add_pending_goto(char *name, Node *node) {
    PendingGoto *pg = arena_alloc(sizeof(PendingGoto));
    pg->name = name;
    pg->node = node;
    pg->next = pending_gotos;
    pending_gotos = pg;
}

static void resolve_pending_gotos(char *name, LVar *locals_at_label) {
    for (PendingGoto *pg = pending_gotos; pg; pg = pg->next) {
        if (pg->name == name)
            pg->node->cleanup_end = locals_at_label;
    }
}

static Typedef *find_typedef(Token *tok) {
    for (Typedef *td = typedefs; td; td = td->next)
        if (equal(tok, td->name))
            return td;
    return NULL;
}

void add_typedef(char *name, Type *ty) {
    Typedef *td = arena_alloc(sizeof(Typedef));
    td->name = name;
    td->ty = ty;
    td->next = typedefs;
    typedefs = td;
}

void init_builtins(void) {
    add_typedef("wchar_t",
#ifdef _WIN32
                ty_ushort
#else
                ty_uint
#endif
    );
    add_typedef("iconv_t", pointer_to(ty_void));
}

static Type *typedef_find_name(const char *name) {
    Token tok = {};
    tok.name = (char *)name;
    Typedef *td = find_typedef(&tok);
    return td ? td->ty : NULL;
}

static EnumConst *find_enum_const(Token *tok) {
    for (EnumConst *ec = enum_consts; ec; ec = ec->next)
        if (equal(tok, ec->name))
            return ec;
    return NULL;
}

static TagScope *find_tag(Token *tok) {
    for (TagScope *tag = tags; tag; tag = tag->next)
        if (equal(tok, tag->name))
            return tag;
    return NULL;
}

static TagScope *push_tag(char *name, Type *ty) {
    TagScope *tag = arena_alloc(sizeof(TagScope));
    tag->name = name;
    tag->ty = ty;
    tag->next = tags;
    tags = tag;
    return tag;
}

static Member *find_member_by_name(Type *ty, char *name) {
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        return NULL;
    for (Member *mem = ty->members; mem; mem = mem->next) {
        if (!mem->name) {
            // Anonymous struct/union: search inside recursively
            if (mem->ty && (mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION)) {
                Member *found = find_member_by_name(mem->ty, name);
                if (found) {
                    // Return synthetic member with combined offset
                    Member *syn = arena_alloc(sizeof(Member));
                    *syn = *found;
                    syn->offset += mem->offset;
                    return syn;
                }
            }
            continue;
        }
        if (strcmp(mem->name, name) == 0)
            return mem;
    }
    return NULL;
}

static Member *find_member(Type *ty, Token *tok) {
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        error_tok(tok, "not a struct or union");
    return find_member_by_name(ty, tok->name);
}

static StrLit *new_str_lit(char *str, int len, int prefix, int elem_size) {
    StrLit *s = arena_alloc(sizeof(StrLit));
    s->str = str;
    s->len = len;
    s->id = str_lit_counter++;
    s->prefix = prefix;
    s->elem_size = elem_size;
    s->next = str_lits;
    str_lits = s;
    return s;
}

static Node *make_cleanup_stmt(LVar *var, Token *tok) {
    Node *call = new_node(ND_FUNCALL, tok);
    call->funcname = var->cleanup_func;
    LVar *fn = find_global_name(var->cleanup_func);
    if (fn)
        call->lhs = new_var_node(fn, tok);
    call->args = new_unary(ND_ADDR, new_var_node(var, tok), tok);

    Node *stmt = new_unary(ND_EXPR_STMT, call, tok);
    add_type(stmt);
    return stmt;
}

// Append cleanup stmts to a node list in-place; returns the (possibly new) list head.
static Node *append_cleanup_flat(Node *body, LVar *begin, LVar *end, Token *tok) {
    Node head = {};
    Node *cur = &head;
    head.next = body;
    while (cur->next)
        cur = cur->next;
    for (LVar *var = begin; var && var != end; var = var->next)
        if (var->is_local && var->cleanup_func)
            cur = cur->next = make_cleanup_stmt(var, tok);
    return head.next;
}

static Node *append_cleanup_range(Node *body, LVar *begin, LVar *end, Token *tok) {
    Node head = {};
    Node *cur = &head;

    if (body) {
        head.next = body;
        while (cur->next)
            cur = cur->next;
    }

    for (LVar *var = begin; var && var != end; var = var->next) {
        if (var->is_local && var->cleanup_func)
            cur = cur->next = make_cleanup_stmt(var, tok);
        if (var->is_local && var->ty->kind == TY_VLA) {
            Node *v = new_node(ND_EXPR_STMT, tok);
            Node *a = new_node(ND_ALLOCA, tok);
            a->kind = ND_ALLOCA_ZINIT;
            a->var = var;
            a->lhs = new_num(0, tok);
            v->lhs = a;
            cur = cur->next = v;
        }
    }

    if (!head.next)
        return body;

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    return node;
}

static bool is_storage_class(Token *tok) {
    return equalc(tok, "typedef") || equalc(tok, "extern") || equalc(tok, "static") ||
        equalc(tok, "inline") || equalc(tok, "__inline") || equalc(tok, "__inline__") ||
        equalc(tok, "register") || equalc(tok, "auto") ||
        equalc(tok, "const") || equalc(tok, "volatile") || equalc(tok, "restrict") ||
        equalc(tok, "__restrict") || equalc(tok, "__restrict__") ||
        equalc(tok, "signed") ||
        equalc(tok, "unsigned") || equalc(tok, "short") || equalc(tok, "long");
}

static Token *skip_balanced(Token *tok) {
    int depth = 0;
    do {
        if (equalc(tok, "("))
            depth++;
        else if (equalc(tok, ")"))
            depth--;
        tok = tok->next;
    } while (depth > 0 && tok->kind != TK_EOF);
    return tok;
}

static Type *type_name(Token **rest, Token *tok);

static Token *read_type_attrs(Token *tok, int *align, VarAttr *attr);

static Token *skip_attributes(Token *tok) {
    return read_type_attrs(tok, NULL, NULL);
}

static unsigned char collect_type_quals(Token **rest, Token *tok) {
    unsigned char q = 0;
    while (true) {
        if (equalc(tok, "const"))
            q |= QUAL_CONST;
        else if (equalc(tok, "volatile"))
            q |= QUAL_VOLATILE;
        else if (equalc(tok, "restrict") || equalc(tok, "__restrict") || equalc(tok, "__restrict__"))
            q |= QUAL_RESTRICT;
        else if (equalc(tok, "_Atomic"))
            q |= QUAL_ATOMIC;
        else
            break;
        tok = tok->next;
    }
    *rest = tok;
    return q;
}


static bool is_typename(Token *tok) {
    if (equalc(tok, "__attribute__") || equalc(tok, "__attribute") ||
        equalc(tok, "__declspec") || equalc(tok, "_Alignas"))
        return true;
    tok = skip_attributes(tok);
    if (equalc(tok, "int") || equalc(tok, "char") || equalc(tok, "void") ||
        equalc(tok, "float") || equalc(tok, "double") ||
        equalc(tok, "_Bool") || equalc(tok, "struct") || equalc(tok, "union") || equalc(tok, "enum") ||
        equalc(tok, "typeof") || equalc(tok, "__typeof") || equalc(tok, "__typeof__") ||
        equalc(tok, "_Atomic"))
        return true;
    if (is_storage_class(tok))
        return true;
    return find_typedef(tok) != NULL;
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Node *expr(Token **rest, Token *tok);
static bool eval_const_expr(Node *node, long long *val);
static bool eval_const_addr_expr(Node *node, long long *val);
static void global_initializer(Token **rest, Token *tok, LVar *var);

static void maybe_update_align(int *align, int value) {
    if (align && value > *align)
        *align = value;
}

static Token *read_type_attrs(Token *tok, int *align, VarAttr *attr) {
    while (true) {
        if (equalc(tok, "_Alignas")) {
            tok = tok->next;
            tok = skip(tok, "(");
            if (is_typename(tok)) {
                Type *ty = type_name(&tok, tok);
                maybe_update_align(align, ty->align);
            } else {
                Node *node = expr(&tok, tok);
                long long val = 0;
                if (!eval_const_expr(node, &val))
                    error_tok(tok, "expected alignment");
                maybe_update_align(align, (int)val);
            }
            tok = skip(tok, ")");
            continue;
        }

        if (equalc(tok, "__asm__") || equalc(tok, "__asm") || equalc(tok, "asm")) {
            Token *start = tok;
            tok = tok->next;
            // Before consuming "(", check if next is "(" — if not, it's not an asm attribute
            if (!equalc(tok, "(")) {
                // Could be __asm__ statement without parens (but we don't handle that here)
                tok = start;
                return tok;
            }
            tok = skip(tok, "(");
            char asm_buf[256];
            asm_buf[0] = '\0';
            int asm_len = 0;
            while (tok->kind == TK_STR || (tok->kind == TK_IDENT && equalc(tok, "_"))) {
                if (tok->kind == TK_STR && asm_len + tok->len < (int)sizeof(asm_buf) - 1) {
                    memcpy(asm_buf + asm_len, tok->str, tok->len);
                    asm_len += tok->len;
                    asm_buf[asm_len] = '\0';
                }
                tok = tok->next;
            }
            // Simple __asm__("label") for symbol naming — no operands
            if (equalc(tok, ")")) {
                tok = skip(tok, ")");
                if (asm_len > 0)
                    pending_asm_name = str_intern(asm_buf, asm_len);
                continue;
            }
            // Inline asm with operand sections — don't consume, let stmt() handle it
            tok = start;
            return tok;
        }

        if (equalc(tok, "__attribute__") || equalc(tok, "__attribute")) {
            tok = tok->next;
            tok = skip(tok, "(");
            tok = skip(tok, "(");
            while (!(equalc(tok, ")") && equalc(tok->next, ")"))) {
                if (equalc(tok, "__cleanup__") || equalc(tok, "cleanup")) {
                    pending_cleanup_tok = tok;
                    tok = tok->next;
                    tok = skip(tok, "(");
                    if (tok->kind == TK_IDENT)
                        pending_cleanup_func = tok->name;
                    tok = tok->next;
                    tok = skip(tok, ")");
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "aligned") || equalc(tok, "__aligned__")) {
                    tok = tok->next;
                    if (equalc(tok, "(")) {
                        tok = tok->next;
                        if (is_typename(tok)) {
                            Type *ty = type_name(&tok, tok);
                            maybe_update_align(align, ty->align);
                        } else {
                            Node *node = expr(&tok, tok);
                            long long val = 0;
                            if (!eval_const_expr(node, &val))
                                error_tok(tok, "expected alignment");
                            maybe_update_align(align, (int)val);
                        }
                        tok = skip(tok, ")");
                    }
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "weak")) {
                    if (attr)
                        attr->is_weak = true;
                    tok = tok->next;
                    if (equalc(tok, "("))
                        tok = skip_balanced(tok);
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "ms_struct")) {
                    if (attr)
                        attr->bitfield_mode = BF_MODE_MS;
                    tok = tok->next;
                    if (equalc(tok, "("))
                        tok = skip_balanced(tok);
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "gcc_struct")) {
                    if (attr)
                        attr->bitfield_mode = BF_MODE_GCC;
                    tok = tok->next;
                    if (equalc(tok, "("))
                        tok = skip_balanced(tok);
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "constructor") || equalc(tok, "__constructor__")) {
                    pending_constructor = true;
                    tok = tok->next;
                    if (equalc(tok, "("))
                        tok = skip_balanced(tok);
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "destructor") || equalc(tok, "__destructor__")) {
                    pending_destructor = true;
                    tok = tok->next;
                    if (equalc(tok, "("))
                        tok = skip_balanced(tok);
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "alias") || equalc(tok, "__alias__")) {
                    tok = tok->next;
                    tok = skip(tok, "(");
                    if (tok->kind == TK_STR) {
                        int len = tok->len;
                        if (len >= 2 && (tok->str[0] == '"' || tok->str[0] == '\''))
                            len -= 2;
                        char *target = malloc(len + 1);
                        if (len >= 2 && (tok->str[0] == '"' || tok->str[0] == '\''))
                            memcpy(target, tok->str + 1, len);
                        else
                            memcpy(target, tok->str, len + 1);
                        target[len] = '\0';
                        pending_alias_target = str_intern(target, len);
                        free(target);
                    }
                    tok = tok->next;
                    tok = skip(tok, ")");
                    if (equalc(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equalc(tok, "(")) {
                    tok = skip_balanced(tok);
                } else {
                    tok = tok->next;
                }

                if (equalc(tok, ","))
                    tok = tok->next;
            }
            tok = skip(tok, ")");
            tok = skip(tok, ")");
            continue;
        }

        if (equalc(tok, "__declspec")) {
            tok = tok->next;
            if (equalc(tok, "("))
                tok = skip_balanced(tok);
            continue;
        }

        break;
    }

    return tok;
}

static bool eval_const_addr_expr(Node *node, long long *val) {
    if (!node) return false;
    switch (node->kind) {
    case ND_MEMBER: {
        long long base_val;
        if (!eval_const_addr_expr(node->lhs, &base_val))
            return false;
        *val = base_val + node->member->offset;
        return true;
    }
    case ND_DEREF:
        return eval_const_expr(node->lhs, val);
    default:
        return eval_const_expr(node, val);
    }
}

static bool eval_const_expr(Node *node, long long *val) {
    long long lhs;
    long long rhs;

    if (!node)
        return false;

    switch (node->kind) {
    case ND_NUM:
        *val = node->val;
        return true;
    case ND_ADD:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs + rhs), true);
    case ND_SUB:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs - rhs), true);
    case ND_MUL:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs * rhs), true);
    case ND_DIV:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && rhs != 0 && ((*val = lhs / rhs), true);
    case ND_MOD:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && rhs != 0 && ((*val = lhs % rhs), true);
    case ND_SHL:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs << rhs), true);
    case ND_SHR:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs >> rhs), true);
    case ND_BITAND:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs & rhs), true);
    case ND_BITXOR:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs ^ rhs), true);
    case ND_BITOR:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs | rhs), true);
    case ND_EQ:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs == rhs), true);
    case ND_NE:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs != rhs), true);
    case ND_LT:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs < rhs), true);
    case ND_LE:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs <= rhs), true);
    case ND_LOGAND:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs && rhs), true);
    case ND_LOGOR:
        return eval_const_expr(node->lhs, &lhs) && eval_const_expr(node->rhs, &rhs) && ((*val = lhs || rhs), true);
    case ND_NEG:
        return eval_const_expr(node->lhs, &lhs) && ((*val = -lhs), true);
    case ND_NOT:
        return eval_const_expr(node->lhs, &lhs) && ((*val = !lhs), true);
    case ND_BITNOT:
        return eval_const_expr(node->lhs, &lhs) && ((*val = ~lhs), true);
    case ND_CAST:
        return eval_const_expr(node->lhs, val);
    case ND_SIZEOF:
        if (node->lhs && node->lhs->ty)
            return (*val = node->lhs->ty->size), true;
        if (node->ty)
            return (*val = node->ty->size), true;
        return false;
    case ND_ADDR:
        // &*x = x
        if (node->lhs->kind == ND_DEREF)
            return eval_const_expr(node->lhs->lhs, val);
        // offsetof: &((struct S*)0)->member
        return eval_const_addr_expr(node->lhs, val);
    case ND_COMMA:
        return eval_const_expr(node->rhs, val);
    case ND_COND:
        if (!eval_const_expr(node->cond, &lhs))
            return false;
        return eval_const_expr(lhs ? node->then : node->els, val);
    default:
        return false;
    }
}

static bool eval_double_const_expr(Node *node, double *val) {
    double lhs, rhs;
    if (!node)
        return false;
    switch (node->kind) {
    case ND_FNUM:
        *val = node->fval;
        return true;
    case ND_NUM:
        *val = (double)node->val;
        return true;
    case ND_ADD:
        if (!eval_double_const_expr(node->lhs, &lhs) || !eval_double_const_expr(node->rhs, &rhs)) return false;
        *val = lhs + rhs;
        if (node->ty && node->ty->kind == TY_FLOAT) *val = (float)*val;
        return true;
    case ND_SUB:
        if (!eval_double_const_expr(node->lhs, &lhs) || !eval_double_const_expr(node->rhs, &rhs)) return false;
        *val = lhs - rhs;
        if (node->ty && node->ty->kind == TY_FLOAT) *val = (float)*val;
        return true;
    case ND_MUL:
        if (!eval_double_const_expr(node->lhs, &lhs) || !eval_double_const_expr(node->rhs, &rhs)) return false;
        *val = lhs * rhs;
        if (node->ty && node->ty->kind == TY_FLOAT) *val = (float)*val;
        return true;
    case ND_DIV:
        if (!eval_double_const_expr(node->lhs, &lhs) || !eval_double_const_expr(node->rhs, &rhs)) return false;
        *val = lhs / rhs;
        if (node->ty && node->ty->kind == TY_FLOAT) *val = (float)*val;
        return true;
    case ND_NEG:
        return eval_double_const_expr(node->lhs, &lhs) && ((*val = -lhs), true);
    case ND_CAST:
        if (!eval_double_const_expr(node->lhs, val)) return false;
        if (node->ty && node->ty->kind == TY_FLOAT) *val = (float)*val;
        if (node->ty && is_integer(node->ty)) *val = (double)(int64_t)*val;
        return true;
    case ND_EQ:
        return eval_double_const_expr(node->lhs, &lhs) && eval_double_const_expr(node->rhs, &rhs) && ((*val = lhs == rhs), true);
    case ND_NE:
        return eval_double_const_expr(node->lhs, &lhs) && eval_double_const_expr(node->rhs, &rhs) && ((*val = lhs != rhs), true);
    case ND_LT:
        return eval_double_const_expr(node->lhs, &lhs) && eval_double_const_expr(node->rhs, &rhs) && ((*val = lhs < rhs), true);
    case ND_LE:
        return eval_double_const_expr(node->lhs, &lhs) && eval_double_const_expr(node->rhs, &rhs) && ((*val = lhs <= rhs), true);
    default:
        return false;
    }
}

static Type *declarator(Token **rest, Token *tok, Type *ty, char **name);

static Type *declarator_params(Token **rest, Token *tok, Type *ty) {
    Type param_head = {};
    Type *pcur = &param_head;
    bool is_variadic = false;

    if (equalc(tok, "void") && equalc(tok->next, ")")) {
        tok = tok->next->next;
    } else {
        while (!equalc(tok, ")")) {
            if (pcur != &param_head)
                tok = skip(tok, ",");
            if (equalc(tok, "...")) {
                is_variadic = true;
                tok = tok->next;
                break;
            }
            if (equalc(tok, ";")) {
                tok = tok->next;
                continue;
            }

            VarAttr attr = {};
            Type *base = declspec(&tok, tok, &attr);
            char *pname = NULL;
            Type *pty = declarator(&tok, tok, copy_type(base), &pname);
            tok = skip_attributes(tok);

            if (pty->kind == TY_ARRAY)
                pty = pointer_to(pty->base);
            else if (pty->kind == TY_FUNC)
                pty = pointer_to(pty);

            Type *pt = arena_alloc(sizeof(Type));
            *pt = *pty;
            pt->param_next = NULL;
            pt->name = pname;
            pcur = pcur->param_next = pt;
        }
        tok = skip(tok, ")");
    }

    ty = func_type(ty);
    ty->param_types = param_head.param_next;
    ty->is_variadic = is_variadic;
    *rest = tok;
    return ty;
}

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
    int dims[16];
    Node *vla_exprs[16] = {0};
    int ndims = 0;
    while (equalc(tok, "[")) {
        tok = tok->next;
        int len = 0;
        Node *vla_expr = NULL;
        while (equalc(tok, "const") || equalc(tok, "volatile") || equalc(tok, "restrict") || equalc(tok, "static"))
            tok = tok->next;
        if (!equalc(tok, "]")) {
            if (equalc(tok, "*")) {
                tok = tok->next;
            } else {
                Node *node = expr(&tok, tok);
                long long val = 0;
                if (eval_const_expr(node, &val)) {
                    len = (int)val;
                } else {
                    len = -1;
                    vla_expr = node;
                }
            }
        }
        tok = skip(tok, "]");
        if (ndims >= 16)
            error_tok(tok, "too many array dimensions");
        dims[ndims] = len;
        vla_exprs[ndims] = vla_expr;
        ndims++;
    }
    // Apply dimensions from innermost (rightmost in source) to outermost
    for (int i = ndims - 1; i >= 0; i--) {
        if (vla_exprs[i])
            ty = vla_of(ty, vla_exprs[i], 0);
        else
            ty = array_of(ty, dims[i]);
    }

    if (equalc(tok, "(")) {
        Token *next = tok->next;
        // Detect old-style (K&R) parameter lists: identifier-only params.
        // If the first token inside () is an identifier (not a type name)
        // followed by ) or ,, leave it for the caller (parse/declaration)
        // to handle, so K&R function definitions are not mis-parsed here.
        if (next->kind == TK_IDENT && !is_typename(next) &&
            (equalc(next->next, ")") || equalc(next->next, ","))) {
            *rest = tok;
            return ty;
        }
        return declarator_params(rest, tok->next, ty);
    }

    *rest = tok;
    return ty;
}

static Type *declarator(Token **rest, Token *tok, Type *ty, char **name) {
    int decl_align = 0;
    tok = read_type_attrs(tok, &decl_align, NULL);
    while (equalc(tok, "*")) {
        ty = pointer_to(ty);
        tok = tok->next;
        tok = read_type_attrs(tok, &decl_align, NULL);
        unsigned char pq = collect_type_quals(&tok, tok);
        if (pq) {
            ty = copy_type(ty);
            ty->qual |= pq;
        }
    }

    Token *inner = tok->next;
    while (equalc(inner, "__cdecl") || equalc(inner, "__stdcall") || equalc(inner, "__fastcall") ||
           equalc(inner, "__thiscall") || equalc(inner, "__vectorcall"))
        inner = inner->next;
    inner = skip_attributes(inner);
    if (equalc(tok, "(")) {
        Token *start = tok->next;
        // Find the matching ) for the initial (
        Token *after_paren = start;
        int depth = 1;
        while (depth > 0 && after_paren->kind != TK_EOF) {
            if (equalc(after_paren, "(")) depth++;
            else if (equalc(after_paren, ")"))
                depth--;
            after_paren = after_paren->next;
        }
        tok = after_paren;
        Type *suffixed = type_suffix(&tok, tok, ty);
        *rest = tok;
        return declarator(&tok, start, suffixed, name);
    }

    // Skip calling convention keywords, attributes, and pointer declarators before the identifier
    for (;;) {
        while (equalc(tok, "__cdecl") || equalc(tok, "__stdcall") || equalc(tok, "__fastcall") ||
               equalc(tok, "__thiscall") || equalc(tok, "__vectorcall"))
            tok = tok->next;
        tok = read_type_attrs(tok, &decl_align, NULL);
        if (equalc(tok, "*")) {
            ty = pointer_to(ty);
            tok = tok->next;
            continue;
        }
        break;
    }

    // asm/__asm__/__asm is not a declarator identifier — let stmt() handle inline asm
    if (equalc(tok, "asm") || equalc(tok, "__asm__") || equalc(tok, "__asm")) {
        if (name) *name = NULL;
        *rest = tok;
        return ty;
    }

    if (tok->kind != TK_IDENT) {
        if (name)
            *name = NULL;
        ty = type_suffix(rest, tok, ty);
        return apply_type_align(ty, decl_align);
    }

    if (name)
        *name = tok->name;
    tok = tok->next;
    tok = read_type_attrs(tok, &decl_align, NULL);
    ty = type_suffix(rest, tok, ty);
    return apply_type_align(ty, decl_align);
}

static Type *enum_specifier(Token **rest, Token *tok) {
    tok = skip(tok, "enum");
    if (tok->kind == TK_IDENT)
        tok = tok->next;

    if (!equalc(tok, "{")) {
        *rest = tok;
        Type *ety = arena_alloc(sizeof(Type));
        *ety = *ty_int;
        ety->is_enum = true;
        return ety;
    }

    tok = tok->next;
    int val = 0;
    while (!equalc(tok, "}")) {
        if (tok->kind != TK_IDENT)
            error_tok(tok, "expected enum constant");

        EnumConst *ec = arena_alloc(sizeof(EnumConst));
        ec->name = tok->name;
        tok = tok->next;

        if (equalc(tok, "=")) {
            tok = tok->next;
            Node *node = conditional(&tok, tok);
            add_type(node);
            long long v = 0;
            if (!eval_const_expr(node, &v))
                error_tok(tok, "expected constant expression for enum value");
            val = v;
        }

        ec->val = val++;
        ec->next = enum_consts;
        enum_consts = ec;

        if (!equalc(tok, "}"))
            tok = skip(tok, ",");
    }

    *rest = tok->next;
    Type *ety = arena_alloc(sizeof(Type));
    *ety = *ty_int;
    ety->is_enum = true;
    return ety;
}

static Type *struct_or_union_specifier(Token **rest, Token *tok, bool is_union) {
    tok = tok->next;
    int struct_attr_align = 0;
    VarAttr struct_attr = {};
    tok = read_type_attrs(tok, &struct_attr_align, &struct_attr);
    char *type_cleanup = pending_cleanup_func;
    pending_cleanup_func = NULL;
    pending_cleanup_tok = NULL;

    Token *tag_tok = NULL;
    if (tok->kind == TK_IDENT) {
        tag_tok = tok;
        tok = tok->next;
    }

    Type *ty = NULL;
    if (tag_tok) {
        TagScope *tag = find_tag(tag_tok);
        if (tag && !equalc(tok, "{")) {
            // Forward-reference use: return existing type
            ty = tag->ty;
        } else if (tag && equalc(tok, "{") && tag->ty->size == 0 && !tag->ty->members) {
            // Completing a forward-declared (incomplete) type: reuse existing Type object
            // so all pointers to it (typedefs, etc.) see the completed definition.
            ty = tag->ty;
        } else {
            // New definition (possibly shadowing an outer-scope tag)
            ty = arena_alloc(sizeof(Type));
            ty->kind = is_union ? TY_UNION : TY_STRUCT;
            ty->size = 0;
            ty->align = 1;
            ty->bitfield_mode = struct_attr.bitfield_mode;
            push_tag(tag_tok->name, ty);
        }
    } else {
        ty = arena_alloc(sizeof(Type));
        ty->kind = is_union ? TY_UNION : TY_STRUCT;
        ty->size = 0;
        ty->align = 1;
        ty->bitfield_mode = struct_attr.bitfield_mode;
    }

    if (struct_attr.bitfield_mode)
        ty->bitfield_mode = struct_attr.bitfield_mode;

    if (!equalc(tok, "{")) {
        *rest = tok;
        return ty;
    }

    tok = tok->next;
    Member head = {};
    Member *cur = &head;
    int offset = 0;
    int max_size = 0;
    int max_align = 1;
    int bit_pos = 0; // current bit position within the struct (for bitfield packing)
    int struct_pack = pack_align; // capture #pragma pack value at struct start
    bool use_ms_bitfields = false;
    if (!is_union) {
        if (ty->bitfield_mode == BF_MODE_MS)
            use_ms_bitfields = true;
        else if (ty->bitfield_mode == BF_MODE_GCC)
            use_ms_bitfields = false;
        else
            use_ms_bitfields = opt_ms_bitfields;
    }
    int ms_run_base = 0;
    TypeKind ms_prev_kind = TY_FUNC;
    int ms_prev_bit_width = 0;

    while (!equalc(tok, "}")) {
        VarAttr attr = {};
        pending_constructor = false;
        pending_destructor = false;
        pending_asm_name = NULL;
        pending_alias_target = NULL;
        Type *base = declspec(&tok, tok, &attr);
        if (attr.is_typedef || attr.is_extern || attr.is_static)
            error_tok(tok, "invalid storage class in member declaration");
        if (!base)
            error_tok(tok, "expected member type");
        if (equalc(tok, ";")) {
            // Anonymous struct/union member: struct { ... }; or union { ... };
            if (base->kind == TY_STRUCT || base->kind == TY_UNION) {
                // bf_unit_size = 0;
                Member *mem = arena_alloc(sizeof(Member));
                mem->name = NULL;
                mem->bit_offset = 0;
                mem->bit_width = 0;
                mem->ty = base;
                if (is_union) {
                    mem->offset = 0;
                    if (max_size < base->size) max_size = base->size;
                } else {
                    int a = base->align;
                    if (struct_pack > 0 && (struct_pack < a || a == 0)) a = struct_pack;
                    offset = align_to(offset, a);
                    mem->offset = offset;
                    offset += base->size;
                    bit_pos = offset * 8;
                    if (max_align < a) max_align = a;
                }
                cur = cur->next = mem;
            }
            tok = tok->next;
            continue;
        }

        for (;;) {
            char *name = NULL;
            Type *mem_ty = declarator(&tok, tok, copy_type(base), &name);
            tok = skip_attributes(tok);

            // Check for bitfield
            int bit_width = 0;
            if (equalc(tok, ":")) {
                tok = tok->next;
                Node *width_node = conditional(&tok, tok);
                long long w;
                if (!eval_const_expr(width_node, &w))
                    error_tok(tok, "bitfield width must be a constant expression");
                bit_width = (int)w;
                if (bit_width < 0 || bit_width > mem_ty->size * 8)
                    error_tok(tok, "bitfield width out of range");
            }

            // Anonymous struct/union member (no name, no bitfield width, aggregate type)
            // e.g. "struct { u8 a, b; };" inside another struct/union
            if (!name && bit_width == 0 && (mem_ty->kind == TY_STRUCT || mem_ty->kind == TY_UNION)) {
                // bf_unit_size = 0;
                Member *mem = arena_alloc(sizeof(Member));
                mem->name = NULL;
                mem->bit_offset = 0;
                mem->bit_width = 0;
                mem->ty = mem_ty;
                if (is_union) {
                    mem->offset = 0;
                    if (max_size < mem_ty->size) max_size = mem_ty->size;
                } else {
                    int a = mem_ty->align;
                    if (struct_pack > 0 && (struct_pack < a || a == 0)) a = struct_pack;
                    offset = align_to(offset, a);
                    mem->offset = offset;
                    offset += mem_ty->size;
                    bit_pos = offset * 8;
                    if (max_align < a) max_align = a;
                }
                cur = cur->next = mem;
                if (!equalc(tok, ",")) break;
                tok = tok->next;
                continue;
            }

            // Handle anonymous bitfield: "int : N" or "int : 0"
            // These don't create named members but affect layout
            if (!name && bit_width >= 0) {
                if (!is_union) {
                    int unit = mem_ty->size;
                    int unit_bits = unit * 8;
                    int align = mem_ty->align;
                    if (struct_pack > 0 && struct_pack < align)
                        align = struct_pack;
                    if (use_ms_bitfields) {
                        TypeKind kind = mem_ty->kind;
                        bool new_run = bit_pos + bit_width > unit_bits ||
                            ((bit_width > 0) == (kind != ms_prev_kind));
                        if (new_run) {
                            offset = align_to(offset, align);
                            ms_run_base = offset;
                            bit_pos = 0;
                            if (bit_width > 0 || ms_prev_bit_width > 0)
                                offset += unit;
                        }
                        ms_prev_kind = kind;
                        ms_prev_bit_width = bit_width;
                        bit_pos += bit_width;
                        if (bit_width > 0 && align > max_align)
                            max_align = align;
                    } else if (bit_width == 0) {
                        // :0 always advances to the next T-aligned boundary
                        // (uses declared type size regardless of struct_pack)
                        int unit_base = (bit_pos / unit_bits) * unit;
                        if (bit_pos % unit_bits != 0)
                            unit_base += unit;
                        if (unit_base > offset) offset = unit_base;
                        bit_pos = unit_base * 8;
                        // bf_unit_size = unit;
                    } else {
                        // :N — advance bit_pos by N bits within layout
                        if (struct_pack > 0) {
                            // Dense packing: just advance cursor
                            bit_pos += bit_width;
                        } else {
                            // Non-packed GCC rule: fit within T-aligned unit
                            int unit_base = (bit_pos / unit_bits) * unit;
                            int bit_off = bit_pos - unit_base * 8;
                            if (bit_off + bit_width > unit_bits) {
                                unit_base += unit;
                                bit_pos = unit_base * 8;
                                if (unit_base > offset) offset = unit_base;
                            }
                            bit_pos += bit_width;
                        }
                        int end_byte = (bit_pos + 7) / 8;
                        if (end_byte > offset) offset = end_byte;
                    }
                }
                if (!equalc(tok, ","))
                    break;
                tok = tok->next;
                continue;
            }

            if (!name)
                error_tok(tok, "expected member name");

            Member *mem = arena_alloc(sizeof(Member));
            mem->name = name;
            mem->bit_width = bit_width;
            mem->bf_load_size = 0;

            if (bit_width > 0) {
                int unit = mem_ty->size; // storage unit size in bytes
                int unit_bits = unit * 8;
                int align = mem_ty->align;
                if (struct_pack > 0 && struct_pack < align)
                    align = struct_pack;

                mem->ty = mem_ty;

                if (is_union) {
                    mem->offset = 0;
                    mem->bit_offset = 0;
                    if (max_size < unit) max_size = unit;
                } else if (use_ms_bitfields) {
                    TypeKind kind = mem_ty->kind;
                    bool new_run = bit_pos + bit_width > unit_bits ||
                        ((bit_width > 0) == (kind != ms_prev_kind));
                    if (new_run) {
                        offset = align_to(offset, align);
                        ms_run_base = offset;
                        bit_pos = 0;
                        if (bit_width > 0 || ms_prev_bit_width > 0)
                            offset += unit;
                    }
                    mem->offset = ms_run_base;
                    mem->bit_offset = bit_pos;
                    ms_prev_kind = kind;
                    ms_prev_bit_width = bit_width;
                    bit_pos += bit_width;
                    if (align > max_align)
                        max_align = align;
                } else if (struct_pack > 0) {
                    // Dense packing (#pragma pack): place at current bit cursor,
                    // but if the member has an explicit alignment attribute
                    // (align > natural size), enforce at least byte-alignment
                    // (limited by struct_pack, so e.g. aligned(16) in pack(1) → 1).
                    if (mem_ty->align > mem_ty->size) {
                        int eff = mem_ty->align < struct_pack ? mem_ty->align : struct_pack;
                        if (eff < 1) eff = 1;
                        int aligned_byte = align_to((bit_pos + 7) / 8, eff);
                        bit_pos = aligned_byte * 8;
                        if (aligned_byte > offset) offset = aligned_byte;
                    }
                    int byte_pos = bit_pos / 8;
                    int bit_off = bit_pos % 8;
                    mem->offset = byte_pos;
                    mem->bit_offset = bit_off;
                    // If field crosses its declared type boundary, record larger load size
                    int needed = (bit_off + bit_width + 7) / 8;
                    if (needed > unit) {
                        int ls = unit;
                        while (ls < needed) ls *= 2;
                        mem->bf_load_size = ls;
                    }
                    bit_pos += bit_width;
                    int end_byte = (bit_pos + 7) / 8;
                    if (end_byte > offset) offset = end_byte;
                } else {
                    // GCC T-aligned unit algorithm (non-packed):
                    // Find the T-aligned storage unit that contains bit_pos.
                    // If the bitfield fits within that unit, place it there.
                    // Otherwise advance to the next T-aligned unit.
                    // If the member type has explicit alignment > unit size,
                    // the bitfield must start at that alignment boundary.
                    int unit_base = (bit_pos / unit_bits) * unit;
                    int bit_off = bit_pos - unit_base * 8;
                    if (bit_off + bit_width > unit_bits) {
                        unit_base += unit;
                        bit_off = 0;
                        bit_pos = unit_base * 8;
                    }
                    // Enforce explicit member alignment (e.g. __attribute__((aligned(16))) char a:4)
                    // Only when alignment was explicitly set beyond the type's natural size.
                    if (mem_ty->align > unit) {
                        unit_base = align_to(unit_base, mem_ty->align);
                        bit_off = 0;
                        bit_pos = unit_base * 8;
                    }
                    mem->offset = unit_base;
                    mem->bit_offset = bit_off;
                    // bf_unit_size = unit;
                    // bf_unit_offset = unit_base;
                    if (unit_base > offset) offset = unit_base;
                    bit_pos = unit_base * 8 + bit_off + bit_width;
                    int end_byte = (bit_pos + 7) / 8;
                    if (end_byte > offset) offset = end_byte;
                    if (max_align < mem_ty->align) max_align = mem_ty->align;
                }
                if (max_align < unit)
                    max_align = unit;
            } else {
                // Normal (non-bitfield) member
                mem->ty = mem_ty;
                mem->bit_offset = 0;
                if (is_union) {
                    mem->offset = 0;
                    if (max_size < mem_ty->size)
                        max_size = mem_ty->size;
                    if (max_align < mem_ty->align)
                        max_align = mem_ty->align;
                } else {
                    int a = mem_ty->align;
                    if (struct_pack > 0 && (struct_pack < a || a == 0))
                        a = struct_pack;
                    offset = align_to(offset, a);
                    mem->offset = offset;
                    offset += mem_ty->size;
                    bit_pos = offset * 8;
                    ms_prev_kind = TY_FUNC;
                    ms_prev_bit_width = 0;
                    if (max_align < a)
                        max_align = a;
                }
                if (struct_pack > 0 && ty->pack_align == 0)
                    ty->pack_align = struct_pack;
            }
            if (name)
                cur = cur->next = mem;

            if (!equalc(tok, ","))
                break;
            tok = tok->next;
        }

        tok = skip(tok, ";");
    }

    tok = skip(tok, "}");
    if (type_cleanup)
        ty->cleanup_func = type_cleanup;
    ty->members = head.next;
    int final_align = max_align;
    if (struct_pack > 0 && struct_pack < max_align)
        final_align = struct_pack;
    if (struct_attr_align > final_align)
        final_align = struct_attr_align;
    ty->align = final_align;
    ty->size = is_union ? align_to(max_size, final_align) : align_to(offset, final_align);
    *rest = tok;
    return ty;
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr) {
    Type *ty = NULL;
    bool is_signed = false;
    bool is_unsigned = false;
    bool is_short = false;
    int long_count = 0;
    bool is_int = false;
    bool is_char = false;
    bool is_float = false;
    bool is_double = false;
    bool is_bool = false;
    bool is_void = false;
    int attr_align = 0;
    unsigned char quals = 0;
    memset(attr, 0, sizeof(*attr));

    for (;;) {
        Token *attr_tok = read_type_attrs(tok, &attr_align, attr);
        if (attr_tok != tok) {
            tok = attr_tok;
            continue;
        }

        if (equalc(tok, "typedef")) {
            attr->is_typedef = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "extern")) {
            attr->is_extern = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "static")) {
            attr->is_static = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "inline") || equalc(tok, "__inline") || equalc(tok, "__inline__")) {
            attr->is_inline = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "register") || equalc(tok, "auto")) {
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "const")) {
            quals |= QUAL_CONST;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "volatile")) {
            quals |= QUAL_VOLATILE;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "restrict") || equalc(tok, "__restrict") || equalc(tok, "__restrict__")) {
            quals |= QUAL_RESTRICT;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "_Atomic")) {
            tok = tok->next;
            if (equalc(tok, "(") && is_typename(tok->next)) {
                tok = skip(tok, "(");
                ty = type_name(&tok, tok);
                tok = skip(tok, ")");
                ty = copy_type(ty);
                ty->qual |= QUAL_ATOMIC;
            } else {
                quals |= QUAL_ATOMIC;
            }
            continue;
        }
        if (equalc(tok, "__cdecl") || equalc(tok, "__stdcall") || equalc(tok, "__fastcall") ||
            equalc(tok, "__thiscall") || equalc(tok, "__vectorcall")) {
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "signed")) {
            is_signed = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "unsigned")) {
            is_unsigned = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "short")) {
            is_short = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "long")) {
            long_count++;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "int")) {
            is_int = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "char")) {
            is_char = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "float")) {
            is_float = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "double")) {
            is_double = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "_Bool")) {
            is_bool = true;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "__int64")) {
            long_count = 2;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "void")) {
            is_void = true;
            tok = tok->next;
            continue;
        }

        if (equalc(tok, "typeof") || equalc(tok, "__typeof") || equalc(tok, "__typeof__")) {
            tok = tok->next;
            tok = skip(tok, "(");
            if (is_typename(tok)) {
                ty = type_name(&tok, tok);
            } else {
                Node *node = expr(&tok, tok);
                add_type(node);
                ty = node->ty;
                // Lvalue conversion: strip top-level qualifiers from expression type
                if (ty && ty->qual) {
                    ty = copy_type(ty);
                    ty->qual = 0;
                }
            }
            tok = skip(tok, ")");
            continue;
        }

        Typedef *td = find_typedef(tok);
        if (td) {
            // If we've already seen a built-in type specifier (int, char, etc.)
            // or another typedef/struct/enum type, stop: the typedef name is
            // likely the variable name, not a type specifier.
            if (is_int || is_char || is_short || long_count > 0 || is_float ||
                is_double || is_bool || is_void || is_signed || is_unsigned || ty)
                break;
            ty = td->ty;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "struct")) {
            ty = struct_or_union_specifier(&tok, tok, false);
            continue;
        }
        if (equalc(tok, "union")) {
            ty = struct_or_union_specifier(&tok, tok, true);
            continue;
        }
        if (equalc(tok, "enum")) {
            ty = enum_specifier(&tok, tok);
            continue;
        }
        break;
    }

    if (!ty) {
        if (is_void) {
            ty = ty_void;
        } else if (is_bool) {
            ty = ty_bool;
        } else if (is_float) {
            ty = ty_float;
        } else if (is_double && long_count >= 1) {
            ty = ty_ldouble;
        } else if (is_double) {
            ty = ty_double;
        } else if (is_char) {
            ty = is_unsigned ? ty_uchar : ty_char;
            if (is_char && is_signed && !is_unsigned) {
                ty = copy_type(ty_char);
                ty->is_signed_char = true;
            }
        } else if (is_short) {
            ty = is_unsigned ? ty_ushort : ty_short;
        } else if (long_count >= 2) {
            ty = is_unsigned ? ty_ullong : ty_llong;
        } else if (long_count == 1) {
            ty = is_unsigned ? ty_ulong : ty_long;
        } else if (is_int || is_signed || is_unsigned) {
            ty = is_unsigned ? ty_uint : ty_int;
        } else {
            ty = ty_int;
            warn_tok(tok, "type defaults to int");
        }
    }

    if (!ty)
        error_tok(tok, "expected type name, got kind=%d text='%.20s'", tok->kind, tok->loc);

    ty = apply_type_align(ty, attr_align);
    tok = skip_attributes(tok);
    quals |= collect_type_quals(&tok, tok);
    if (quals) {
        ty = copy_type(ty);
        ty->qual |= quals;
    }
    *rest = tok;
    return ty;
}

static Type *type_name(Token **rest, Token *tok) {
    VarAttr attr = {};
    Type *base = declspec(&tok, tok, &attr);
    Type *ty = declarator(&tok, tok, copy_type(base), NULL);
    tok = skip_attributes(tok);
    *rest = tok;
    return ty;
}

static Type *parse_cast_type(Token **rest, Token *tok) {
    tok = skip(tok, "(");
    Type *ty = type_name(&tok, tok);
    *rest = skip(tok, ")");
    return ty;
}

static bool is_cast(Token *tok) {
    if (!equalc(tok, "("))
        return false;
    tok = tok->next;
    tok = skip_attributes(tok);
    return is_typename(tok);
}

static Node *stmt(Token **rest, Token *tok);
static Node *expr(Token **rest, Token *tok);
static Node *assign(Token **rest, Token *tok);

static int array_len(Type *ty) {
    if (!ty || ty->kind != TY_ARRAY || !ty->base || ty->base->size == 0)
        return 0;
    return ty->size / ty->base->size;
}

static void append_reloc(LVar *var, int offset, char *label, int addend) {
    Reloc *rel = arena_alloc(sizeof(Reloc));
    rel->offset = offset;
    rel->label = label;
    rel->addend = addend;

    if (!var->relocs || var->relocs->offset > offset) {
        rel->next = var->relocs;
        var->relocs = rel;
        return;
    }

    // Replace head if offset matches
    if (var->relocs->offset == offset) {
        rel->next = var->relocs->next;
        var->relocs = rel;
        return;
    }

    Reloc *cur = var->relocs;
    while (cur->next && cur->next->offset < offset)
        cur = cur->next;

    if (cur->next && cur->next->offset == offset) {
        // Replace existing reloc at same offset (designator override)
        rel->next = cur->next->next;
        cur->next = rel;
    } else {
        rel->next = cur->next;
        cur->next = rel;
    }
}

static bool read_global_label_initializer(Token **rest, Token *tok, char **label, int *addend) {
    if (tok->kind == TK_STR) {
        StrLit *s = new_str_lit(tok->str, tok->len, tok->string_literal_prefix, 1);
        *label = format(".LC%d", s->id);
        if (addend) *addend = 0;
        *rest = tok->next;
        return true;
    }

    // GCC label address: &&label
    if (equalc(tok, "&&") && tok->next && tok->next->kind == TK_IDENT) {
        if (parser_current_fn)
            *label = format(".L.label.%s.%s", parser_current_fn, tok->next->name);
        else
            *label = tok->next->name;
        if (addend) *addend = 0;
        *rest = tok->next->next;
        return true;
    }

    if (equalc(tok, "&"))
        tok = tok->next;

    if (tok->kind == TK_IDENT) {
        *label = tok->name;
        if (addend) *addend = 0;
        *rest = tok->next;

        // Handle &identifier[constant] — array subscript in global initializer
        if (equalc(*rest, "[")) {
            Token *sub = (*rest)->next;
            Node *idx = assign(&sub, sub);
            add_type(idx);
            long long ival;
            if (sub->kind != TK_EOF && equalc(sub, "]") && eval_const_expr(idx, &ival)) {
                LVar *var = find_global_name(*label);
                if (var && var->ty && var->ty->kind == TY_ARRAY)
                    ival *= var->ty->base->size;
                if (addend) *addend = (int)ival;
                *rest = sub->next;
            }
        }

        // Handle &identifier.member — struct member access in global initializer
        if (equalc(*rest, ".")) {
            LVar *var = find_global_name(*label);
            Token *member_tok = (*rest)->next;
            if (var && var->ty && member_tok && member_tok->kind == TK_IDENT) {
                Member *mem = find_member(var->ty, member_tok);
                if (mem) {
                    if (addend) *addend += mem->offset;
                    *rest = member_tok->next;
                }
            }
        }

        return true;
    }

    return false;
}

static Token *skip_initializer(Token *tok) {
    // Skip designated initializer: .name = value
    if (equalc(tok, ".") && tok->next && tok->next->kind == TK_IDENT) {
        tok = tok->next->next;
        if (equalc(tok, "="))
            tok = tok->next;
        return skip_initializer(tok);
    }
    // Skip array index designator: [N] = value or [N ... M] = value
    if (equalc(tok, "[")) {
        int depth = 1;
        tok = tok->next;
        while (depth > 0 && tok->kind != TK_EOF) {
            if (equalc(tok, "[")) depth++;
            else if (equalc(tok, "]"))
                depth--;
            tok = tok->next;
        }
        if (equalc(tok, "...")) {
            tok = tok->next;
            while (!equalc(tok, "]") && tok->kind != TK_EOF) tok = tok->next;
            tok = tok->next;
        }
        if (equalc(tok, "=")) tok = tok->next;
        return skip_initializer(tok);
    }
    if (!equalc(tok, "{")) {
        assign(&tok, tok);
        return tok;
    }

    int depth = 0;
    do {
        if (equalc(tok, "{"))
            depth++;
        else if (equalc(tok, "}"))
            depth--;
        tok = tok->next;
    } while (depth > 0 && tok->kind != TK_EOF);
    return tok;
}

static Token *skip_flat_aggregate_init(Token *tok, Type *ty) {
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        Member *mem = ty->members;
        while (mem) {
            if (equalc(tok, "}"))
                break;
            tok = skip_flat_aggregate_init(tok, mem->ty);
            mem = mem->next;
            if (mem && equalc(tok, ","))
                tok = tok->next;
            if (ty->kind == TY_UNION)
                break;
        }
    } else if (ty->kind == TY_ARRAY) {
        if (equalc(tok, "{") || (ty->base->kind == TY_CHAR && tok->kind == TK_STR)) {
            tok = skip_initializer(tok);
        } else {
            int len = array_len(ty);
            for (int i = 0; i < len && !equalc(tok, "}"); i++) {
                tok = skip_flat_aggregate_init(tok, ty->base);
                if (i < len - 1 && equalc(tok, ","))
                    tok = tok->next;
            }
        }
    } else {
        assign(&tok, tok);
    }
    return tok;
}

// Evaluate a constant integer expression without consuming the tokens permanently.
static long long peek_const_expr(Token *tok) {
    Token *tmp = tok;
    Node *node = assign(&tmp, tmp);
    add_type(node);
    long long val = 0;
    if (!eval_const_expr(node, &val))
        return -1;
    return val;
}

static Token *find_compound_literal_start(Token *tok);

static int count_array_initializer(Token **rest, Token *tok, Type *elem_ty) {
    int count = 0;
    int max_idx = -1;
    int idx = 0;
    tok = skip(tok, "{");
    while (!equalc(tok, "}")) {
        int eidx = idx;
        if (equalc(tok, "[")) {
            tok = tok->next; // skip [
            long long aidx = peek_const_expr(tok);
            assign(&tok, tok); // skip first expression
            eidx = (int)aidx;
            if (equalc(tok, "...")) {
                tok = tok->next; // skip ...
                long long aeidx = peek_const_expr(tok);
                assign(&tok, tok); // skip second expression
                eidx = (int)aeidx;
            }
            if (eidx > max_idx) max_idx = eidx;
            tok = skip(tok, "]");
            tok = skip(tok, "=");
        }
        if (elem_ty && (elem_ty->kind == TY_STRUCT || elem_ty->kind == TY_UNION) && !equalc(tok, "{")) {
            // Heuristic: if the first token is an identifier of struct/union type,
            // or a compound literal, treat it as a single element expression.
            // Otherwise use flat aggregate initialization.
            bool is_struct_expr = false;
            if (tok->kind == TK_IDENT) {
                LVar *var = find_var(tok);
                if (var && var->ty && (var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION))
                    is_struct_expr = true;
            } else if (find_compound_literal_start(tok)) {
                is_struct_expr = true;
            }
            if (is_struct_expr) {
                tok = skip_initializer(tok);
            } else {
                tok = skip_flat_aggregate_init(tok, elem_ty);
            }
        } else {
            tok = skip_initializer(tok);
        }
        if (eidx > max_idx) max_idx = eidx;
        count++;
        idx = eidx + 1;
        if (equalc(tok, ",")) {
            tok = tok->next;
            if (equalc(tok, "}"))
                break;
            continue;
        }
        break;
    }
    *rest = skip(tok, "}");
    return max_idx >= count ? max_idx + 1 : count;
}

static Type *infer_array_type(Type *ty, Token *tok) {
    if (!ty || ty->kind != TY_ARRAY || ty->size != 0)
        return ty;
    if (tok->kind == TK_STR) {
        if (tok->string_literal_prefix == 0)
            return array_of(ty->base, tok->len + 1);
        // For wide strings, count UTF-8 characters (each becomes one wchar)
        return array_of(ty->base, utf8_len(tok->str) + 1);
    }
    if (equalc(tok, "{")) {
        Token *tmp = tok;
        int len = count_array_initializer(&tmp, tmp, ty->base);
        return array_of(ty->base, len);
    }
    return ty;
}

// Detect compound literal like (type){...} or ((type){...}) and return
// a pointer to the { token, or NULL if not a compound literal.
static Token *find_compound_literal_start(Token *tok) {
    Token *t = tok;
    while (equalc(t, "("))
        t = t->next;
    if (!is_typename(t))
        return NULL;
    Type *ty = type_name(&t, t);
    if (!ty)
        return NULL;
    while (equalc(t, ")"))
        t = t->next;
    if (equalc(t, "{"))
        return t;
    return NULL;
}

static void remove_reloc(LVar *var, int offset) {
    if (!var->relocs) return;
    if (var->relocs->offset == offset) {
        var->relocs = var->relocs->next;
        return;
    }
    for (Reloc *cur = var->relocs; cur->next; cur = cur->next) {
        if (cur->next->offset == offset) {
            cur->next = cur->next->next;
            return;
        }
    }
}

static void ensure_init_size(LVar *var, int offset, int size) {
    int need = offset + size;
    if (need > var->init_size) {
        char *new_data = arena_alloc(need);
        if (var->init_data) {
            memcpy(new_data, var->init_data, var->init_size);
            memset(new_data + var->init_size, 0, need - var->init_size);
        }
        var->init_data = new_data;
        var->init_size = need;
    }
}

static void write_scalar_bytes(LVar *var, int offset, int size, int64_t val) {
    if (offset < 0) return;
    ensure_init_size(var, offset, size);
    // Remove any reloc at this offset (scalar value overrides pointer reloc)
    remove_reloc(var, offset);
    if (size == 1) {
        var->init_data[offset] = (char)val;
        return;
    }
    if (size == 2) {
        int16_t v = (int16_t)val;
        memcpy(var->init_data + offset, &v, 2);
        return;
    }
    if (size == 4) {
        int32_t v = (int32_t)val;
        memcpy(var->init_data + offset, &v, 4);
        return;
    }
    int64_t v = val;
    memcpy(var->init_data + offset, &v, 8);
}

// Forward declaration
static Token *global_init_one(Token *tok, LVar *var, Type *ty, int offset);

static Token *global_init_flat_array(Token *tok, LVar *var, Type *ty, int offset) {
    if (ty->kind == TY_ARRAY) {
        int len = array_len(ty);
        Type *base = ty->base;
        int elem_size = base->size;
        for (int i = 0; i < len && !equalc(tok, "}"); i++) {
            tok = global_init_flat_array(tok, var, base, offset + i * elem_size);
            if (i < len - 1 && equalc(tok, ","))
                tok = tok->next;
        }
        return tok;
    }
    return global_init_one(tok, var, ty, offset);
}

static Token *global_init_member(Token *tok, LVar *var, Member *mem, int base_offset) {
    if (mem->bit_width > 0) {
        Node *node = assign(&tok, tok);
        add_type(node);
        long long val = 0;
        if (eval_const_expr(node, &val)) {
            int off = base_offset + mem->offset;
            int unit_sz = mem->ty->size;
            unsigned long long mask;
            unsigned long long new_val;
            if (mem->bit_width == 64) {
                mask = ~0ULL << mem->bit_offset;
                new_val = val << mem->bit_offset;
            } else {
                mask = ((1ULL << mem->bit_width) - 1) << mem->bit_offset;
                new_val = ((val & ((1ULL << mem->bit_width) - 1)) << mem->bit_offset);
            }
            if (unit_sz == 1) {
                unsigned char old = var->init_data[off];
                var->init_data[off] = (old & ~mask) | new_val;
            } else if (unit_sz == 2) {
                uint16_t old;
                memcpy(&old, var->init_data + off, 2);
                old = (old & ~mask) | new_val;
                memcpy(var->init_data + off, &old, 2);
            } else if (unit_sz == 4) {
                uint32_t old;
                memcpy(&old, var->init_data + off, 4);
                old = (old & ~mask) | new_val;
                memcpy(var->init_data + off, &old, 4);
            } else {
                uint64_t old;
                memcpy(&old, var->init_data + off, 8);
                old = (old & ~mask) | new_val;
                memcpy(var->init_data + off, &old, 8);
            }
        }
        return tok;
    }
    if (mem->ty->kind == TY_ARRAY && !equalc(tok, "{") && !(mem->ty->base->kind == TY_CHAR && tok->kind == TK_STR)) {
        return global_init_flat_array(tok, var, mem->ty, base_offset + mem->offset);
    }
    return global_init_one(tok, var, mem->ty, base_offset + mem->offset);
}

// Initialize one object of type `ty` at `base + offset` in global init data.
// Handles scalars, arrays, structs, compound literals, and flattened init.
static Token *global_init_one(Token *tok, LVar *var, Type *ty, int offset) {
    // String literal for char array
    if (ty->kind == TY_ARRAY && ty->base->kind == TY_CHAR && tok->kind == TK_STR) {
        int len = strlen(tok->str) + 1;
        if (ty->size > 0 && len > ty->size) len = ty->size;
        ensure_init_size(var, offset, len);
        memcpy(var->init_data + offset, tok->str, len);
        return tok->next;
    }

    // Array with braces: { elem1, elem2, ... } with optional [N]=val or [N...M]=val designators
    if (ty->kind == TY_ARRAY && equalc(tok, "{")) {
        int elem_size = ty->base->size;
        int len = array_len(ty);
        tok = skip(tok, "{");
        int idx = 0;
        while (!equalc(tok, "}")) {
            int sidx = idx, eidx = idx;
            if (equalc(tok, "[")) {
                tok = tok->next;
                Node *n = assign(&tok, tok);
                long long sv = 0;
                eval_const_expr(n, &sv);
                sidx = (int)sv;
                eidx = sidx;
                if (equalc(tok, "...")) {
                    tok = tok->next;
                    Node *n2 = assign(&tok, tok);
                    long long ev = sidx;
                    eval_const_expr(n2, &ev);
                    eidx = (int)ev;
                }
                tok = skip(tok, "]");
                tok = skip(tok, "=");
                idx = sidx;
            }
            Token *val_start = tok;
            for (int i = sidx; i <= eidx; i++) {
                if (len == 0 || i < len)
                    tok = global_init_one(val_start, var, ty->base, offset + i * elem_size);
                else
                    tok = skip_initializer(val_start);
            }
            idx = eidx + 1;
            if (equalc(tok, ",")) {
                tok = tok->next;
                if (equalc(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        return skip(tok, "}");
    }

    // Struct/union with braces: { mem1, mem2, ... }
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && equalc(tok, "{")) {
        tok = skip(tok, "{");
        Member *mem = ty->members;
        while (!equalc(tok, "}")) {
            // Designated initializer: .member = value
            if (equalc(tok, ".") && tok->next && tok->next->kind == TK_IDENT) {
                char *name = tok->next->name;
                tok = tok->next->next;
                tok = skip(tok, "=");
                Member *m = find_member_by_name(ty, name);
                if (m) {
                    tok = global_init_member(tok, var, m, offset);
                    mem = m->next;
                } else {
                    tok = skip_initializer(tok);
                }
            } else if (mem) {
                tok = global_init_member(tok, var, mem, offset);
                mem = mem->next;
            } else {
                tok = skip_initializer(tok);
            }
            if (equalc(tok, ",")) {
                tok = tok->next;
                if (equalc(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        return skip(tok, "}");
    }

    // Compound literal for aggregate type
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_ARRAY) && find_compound_literal_start(tok)) {
        Token *compound_start = find_compound_literal_start(tok);
        tok = skip(compound_start, "{");
        if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
            Member *mem = ty->members;
            while (!equalc(tok, "}")) {
                if (mem) {
                    tok = global_init_member(tok, var, mem, offset);
                    mem = mem->next;
                } else {
                    tok = skip_initializer(tok);
                }
                if (equalc(tok, ",")) {
                    tok = tok->next;
                    if (equalc(tok, "}"))
                        break;
                    continue;
                }
                break;
            }
        } else { // array
            int elem_size = ty->base->size;
            int len = array_len(ty);
            int idx = 0;
            while (!equalc(tok, "}")) {
                if (len == 0 || idx < len)
                    tok = global_init_one(tok, var, ty->base, offset + idx * elem_size);
                else
                    tok = skip_initializer(tok);
                idx++;
                if (equalc(tok, ",")) {
                    tok = tok->next;
                    if (equalc(tok, "}"))
                        break;
                    continue;
                }
                break;
            }
        }
        if (equalc(tok, "}"))
            tok = tok->next;
        while (equalc(tok, ")"))
            tok = tok->next;
        return tok;
    }

    // Struct/union without braces: flatten into members.
    // For unions, only the first member is initialized.
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        Member *mem = ty->members;
        if (mem) {
            tok = global_init_member(tok, var, mem, offset);
            mem = mem->next;
            if (ty->kind == TY_STRUCT) {
                while (mem && !equalc(tok, "}")) {
                    if (equalc(tok, ","))
                        tok = tok->next;
                    if (equalc(tok, "}"))
                        break;
                    tok = global_init_member(tok, var, mem, offset);
                    mem = mem->next;
                }
            }
        }
        return tok;
    }

    // Array without braces: single element
    if (ty->kind == TY_ARRAY) {
        return global_init_one(tok, var, ty->base, offset);
    }

    // Superfluous braces around scalar: { expr }
    if (equalc(tok, "{")) {
        tok = skip(tok, "{");
        tok = global_init_one(tok, var, ty, offset);
        tok = skip(tok, "}");
        return tok;
    }

    // Pointer to label/function
    if (ty->kind == TY_PTR) {
        char *label = NULL;
        int addend = 0;
        Token *next = tok;
        if (read_global_label_initializer(&next, tok, &label, &addend)) {
            append_reloc(var, offset, label, addend);
            return next;
        }
    }

    // Scalar
    Node *node = assign(&tok, tok);
    add_type(node);
    if (is_flonum(ty)) {
        double fv = 0;
        if (eval_double_const_expr(node, &fv)) {
            if (ty->size == 4) {
                float f = (float)fv;
                memcpy(var->init_data + offset, &f, 4);
            } else {
                memcpy(var->init_data + offset, &fv, 8);
            }
            return tok;
        }
        error_tok(tok, "expected constant expression in initializer");
        return tok;
    }
    long long val = 0;
    if (eval_const_expr(node, &val)) {
        write_scalar_bytes(var, offset, ty->size, (int64_t)val);
        return tok;
    }
    error_tok(tok, "expected constant expression in initializer");
    return tok;
}

// Forward declarations for local recursive initializer
static Token *local_init_one(Token *tok, Node *lhs, Type *ty, Node **cur);

static Node *new_array_elem_lvalue_node(Node *base, int idx, Token *tok) {
    Node *offset = new_num(idx, tok);
    Node *add = new_binary(ND_ADD, base, offset, tok);
    add_type(add);
    Node *deref = new_unary(ND_DEREF, add, tok);
    add_type(deref);
    return deref;
}

static Token *local_init_flat_array(Token *tok, Node *lhs, Type *ty, Node **cur) {
    if (ty->kind == TY_ARRAY) {
        int len = array_len(ty);
        Type *base = ty->base;
        for (int i = 0; i < len && !equalc(tok, "}"); i++) {
            Node *elem_lhs = new_array_elem_lvalue_node(lhs, i, tok);
            tok = local_init_flat_array(tok, elem_lhs, base, cur);
            if (i < len - 1 && equalc(tok, ","))
                tok = tok->next;
        }
        return tok;
    }
    return local_init_one(tok, lhs, ty, cur);
}

static Token *local_init_member(Token *tok, Node *lhs, Member *mem, Node **cur) {
    Node *mem_node = new_unary(ND_MEMBER, lhs, tok);
    mem_node->member = mem;
    add_type(mem_node);
    if (mem->ty->kind == TY_ARRAY && !equalc(tok, "{") && !(mem->ty->base->kind == TY_CHAR && tok->kind == TK_STR)) {
        return local_init_flat_array(tok, mem_node, mem->ty, cur);
    }
    return local_init_one(tok, mem_node, mem->ty, cur);
}

static Token *local_init_one(Token *tok, Node *lhs, Type *ty, Node **cur) {
    // String literal for char or wide-char array
    if (ty->kind == TY_ARRAY && tok->kind == TK_STR &&
        (ty->base->kind == TY_CHAR || tok->string_literal_prefix != 0)) {
        Node *rhs = assign(&tok, tok);
        Node *assign_node = new_binary(ND_ASSIGN, lhs, rhs, tok);
        add_type(assign_node);
        *cur = (*cur)->next = new_unary(ND_EXPR_STMT, assign_node, tok);
        return tok;
    }

    // Array with braces
    if (ty->kind == TY_ARRAY && equalc(tok, "{")) {
        int len = array_len(ty);
        tok = skip(tok, "{");
        int idx = 0;
        while (!equalc(tok, "}")) {
            int sidx = idx, eidx = idx;
            if (equalc(tok, "[")) {
                tok = tok->next;
                Node *n = assign(&tok, tok);
                long long sv = 0;
                eval_const_expr(n, &sv);
                sidx = (int)sv;
                eidx = sidx;
                if (equalc(tok, "...")) {
                    tok = tok->next;
                    Node *n2 = assign(&tok, tok);
                    long long ev = sidx;
                    eval_const_expr(n2, &ev);
                    eidx = (int)ev;
                }
                tok = skip(tok, "]");
                tok = skip(tok, "=");
                idx = sidx;
            }
            Token *val_start = tok;
            if (sidx != eidx && !equalc(tok, "{")) {
                // Range with scalar/non-brace value: evaluate once into a temp
                Token *after_val = tok;
                Node *rhs = assign(&after_val, after_val);
                add_type(rhs);
                LVar *tmp = new_var("", rhs->ty, true);
                Node *tmp_assign = new_binary(ND_ASSIGN, new_var_node(tmp, tok), rhs, tok);
                add_type(tmp_assign);
                *cur = (*cur)->next = new_unary(ND_EXPR_STMT, tmp_assign, tok);
                for (int i = sidx; i <= eidx; i++) {
                    if (len == 0 || i < len) {
                        Node *elem_lhs = new_array_elem_lvalue_node(lhs, i, tok);
                        Node *assign_node = new_binary(ND_ASSIGN, elem_lhs,
                                                       new_var_node(tmp, tok), tok);
                        add_type(assign_node);
                        *cur = (*cur)->next = new_unary(ND_EXPR_STMT, assign_node, tok);
                    }
                }
                tok = after_val;
            } else {
                for (int i = sidx; i <= eidx; i++) {
                    if (len == 0 || i < len) {
                        Node *elem_lhs = new_array_elem_lvalue_node(lhs, i, tok);
                        tok = local_init_one(val_start, elem_lhs, ty->base, cur);
                    } else {
                        tok = skip_initializer(val_start);
                    }
                }
            }
            idx = eidx + 1;
            if (equalc(tok, ",")) {
                tok = tok->next;
                if (equalc(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        return skip(tok, "}");
    }

    // Struct/union with braces
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION) && equalc(tok, "{")) {
        tok = skip(tok, "{");
        Member *mem = ty->members;
        while (!equalc(tok, "}")) {
            if (equalc(tok, ".") && tok->next && tok->next->kind == TK_IDENT) {
                // Parse chain of .member designators
                Node *chain_lhs = lhs;
                Type *chain_ty = ty;
                Member *first_dm = NULL;
                Member *last_dm = NULL;
                bool chain_ok = true;
                while (equalc(tok, ".") && tok->next && tok->next->kind == TK_IDENT) {
                    char *dname = tok->next->name;
                    tok = tok->next->next;
                    Member *dm = find_member_by_name(chain_ty, dname);
                    if (!dm) {
                        chain_ok = false;
                        break;
                    }
                    if (!first_dm) first_dm = dm;
                    Node *mem_node = new_unary(ND_MEMBER, chain_lhs, tok);
                    mem_node->member = dm;
                    add_type(mem_node);
                    chain_lhs = mem_node;
                    last_dm = dm;
                    chain_ty = dm->ty;
                }
                tok = skip(tok, "=");
                if (!chain_ok || !last_dm) {
                    tok = skip_initializer(tok);
                } else {
                    tok = local_init_one(tok, chain_lhs, chain_ty, cur);
                }
                mem = first_dm ? first_dm->next : NULL;
            } else if (tok->kind == TK_IDENT && tok->next && equalc(tok->next, ":")) {
                // GNU-style designated init: member: value
                char *name = tok->name;
                tok = tok->next->next;
                Member *m = find_member_by_name(ty, name);
                if (m) {
                    tok = local_init_member(tok, lhs, m, cur);
                    mem = m->next;
                } else {
                    tok = skip_initializer(tok);
                }
            } else if (mem) {
                tok = local_init_member(tok, lhs, mem, cur);
                mem = mem->next;
            } else {
                tok = skip_initializer(tok);
            }
            if (equalc(tok, ",")) {
                tok = tok->next;
                if (equalc(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        return skip(tok, "}");
    }

    // Compound literal for aggregate type
    if ((ty->kind == TY_STRUCT || ty->kind == TY_UNION || ty->kind == TY_ARRAY) && find_compound_literal_start(tok)) {
        Node *rhs = assign(&tok, tok);
        Node *assign_node = new_binary(ND_ASSIGN, lhs, rhs, tok);
        add_type(assign_node);
        *cur = (*cur)->next = new_unary(ND_EXPR_STMT, assign_node, tok);
        return tok;
    }

    // Struct/union without braces: check if single struct expression, else flatten
    if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
        Token *saved = tok;
        Node *node = assign(&saved, saved);
        add_type(node);
        if (node->ty && (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)) {
            tok = saved;
            Node *assign_node = new_binary(ND_ASSIGN, lhs, node, tok);
            add_type(assign_node);
            *cur = (*cur)->next = new_unary(ND_EXPR_STMT, assign_node, tok);
            return tok;
        }
        // Flatten into members
        Member *mem = ty->members;
        if (mem) {
            tok = local_init_member(tok, lhs, mem, cur);
            mem = mem->next;
            if (ty->kind == TY_STRUCT) {
                while (mem && !equalc(tok, "}")) {
                    if (equalc(tok, ","))
                        tok = tok->next;
                    if (equalc(tok, "}"))
                        break;
                    tok = local_init_member(tok, lhs, mem, cur);
                    mem = mem->next;
                }
            }
        }
        return tok;
    }

    // Array without braces: single element
    if (ty->kind == TY_ARRAY) {
        Node *elem_lhs = new_array_elem_lvalue_node(lhs, 0, tok);
        return local_init_one(tok, elem_lhs, ty->base, cur);
    }

    // Superfluous braces around scalar
    if (equalc(tok, "{")) {
        tok = skip(tok, "{");
        tok = local_init_one(tok, lhs, ty, cur);
        tok = skip(tok, "}");
        return tok;
    }

    // Scalar
    Node *rhs = assign(&tok, tok);
    Node *assign_node = new_binary(ND_ASSIGN, lhs, rhs, tok);
    add_type(assign_node);
    *cur = (*cur)->next = new_unary(ND_EXPR_STMT, assign_node, tok);
    return tok;
}

static Node *declaration(Token **rest, Token *tok) {
    VarAttr attr = {};
    pending_cleanup_func = NULL;
    pending_constructor = false;
    pending_destructor = false;
    pending_asm_name = NULL;
    pending_alias_target = NULL;
    Type *base = declspec(&tok, tok, &attr);
    char *type_level_cleanup = pending_cleanup_func;
    Node head = {};
    Node *cur = &head;

    if (equalc(tok, ";")) {
        pending_cleanup_func = NULL;
        *rest = tok->next;
        return new_node(ND_NULL, tok);
    }

    while (!equalc(tok, ";")) {
        char *name = NULL;
        pending_cleanup_func = NULL;
        Type *ty = declarator(&tok, tok, copy_type(base), &name);
        tok = skip_attributes(tok);
        char *cleanup = pending_cleanup_func ? pending_cleanup_func : type_level_cleanup;
        pending_cleanup_func = NULL;
        if (!name)
            error_tok(tok, "expected variable name");

        if (ty->kind == TY_FUNC) {
            Type *fty = ty;
            LVar *fn_sym = find_global_name(name);
            if (!fn_sym) {
                fn_sym = new_var(name, pointer_to(fty), false);
                fn_sym->is_extern = true;
                fn_sym->is_function = true;
                fn_sym->is_weak = attr.is_weak;
            } else {
                if (attr.is_weak)
                    fn_sym->is_weak = true;
            }
            // Create local entry so this function declaration shadows any local variable
            LVar *lvar = arena_alloc(sizeof(LVar));
            lvar->name = name;
            lvar->ty = pointer_to(fty);
            lvar->is_local = false;
            lvar->is_extern = true;
            lvar->is_function = true;
            lvar->is_weak = attr.is_weak;
            if (pending_asm_name)
                lvar->asm_name = pending_asm_name;
            lvar->next = locals;
            locals = lvar;
            if (current_block_depth == 1)
                current_fn_scope_locals = locals;
            if (!equalc(tok, ","))
                break;
            tok = tok->next;
            continue;
        }

        if (attr.is_typedef) {
            add_typedef(name, ty);
        } else if (attr.is_static) {
            // Static local variable: create global storage with unique name
            char *asm_label = format(".Lstatic.%d", static_local_counter++);
            if (equalc(tok, "="))
                ty = infer_array_type(ty, tok->next);
            // Global entry for storage
            LVar *gvar = arena_alloc(sizeof(LVar));
            gvar->name = asm_label;
            gvar->ty = ty;
            gvar->is_local = false;
            gvar->is_static = true;
            gvar->next = globals;
            globals = gvar;
            // Local entry for name lookup
            LVar *lvar = arena_alloc(sizeof(LVar));
            lvar->name = name;
            lvar->asm_name = pending_asm_name ? pending_asm_name : asm_label;
            lvar->ty = ty;
            lvar->is_local = false;
            lvar->is_static = true;
            lvar->next = locals;
            locals = lvar;
            if (equalc(tok, "=")) {
                tok = tok->next;
                global_initializer(&tok, tok, gvar);
                lvar->ty = gvar->ty;
            }
        } else if (attr.is_extern) {
            // Block-scope extern declaration: refers to global storage
            LVar *gvar = find_global_name(name);
            if (!gvar) {
                gvar = new_var(name, ty, false);
                gvar->is_extern = true;
            } else if (gvar->ty->kind == TY_ARRAY && ty->kind == TY_ARRAY && ty->size > 0 && gvar->ty->size == 0) {
                gvar->ty = ty;
            }
            if (pending_asm_name)
                gvar->asm_name = pending_asm_name;
            // Create local entry that references the global
            LVar *lvar = arena_alloc(sizeof(LVar));
            lvar->name = name;
            lvar->ty = gvar->ty;
            lvar->is_local = false;
            lvar->is_extern = true;
            if (pending_asm_name)
                lvar->asm_name = pending_asm_name;
            lvar->next = locals;
            locals = lvar;
            if (current_block_depth == 1)
                current_fn_scope_locals = locals;
        } else {
            if (equalc(tok, "=")) {
                ty = infer_array_type(ty, tok->next);
            }
            LVar *var = new_var(name, ty, true);
            if (pending_asm_name)
                var->asm_name = pending_asm_name;
            var->cleanup_func = cleanup ? cleanup : ty->cleanup_func;

            // VLA: compute size and allocate stack space
            if (ty->kind == TY_VLA) {
                Node *vla_node = new_node(ND_ALLOCA, tok);
                vla_node->lhs = vla_alloc_size(ty, tok);
                vla_node->var = var;
                cur = cur->next = new_unary(ND_EXPR_STMT, vla_node, tok);
                fn_uses_vla = true;
            }

            if (current_block_depth == 1)
                current_fn_scope_locals = locals;
            if (equalc(tok, "=")) {
                Token *start = tok;
                tok = tok->next;
                Node *lhs = new_var_node(var, start);
                // Zero-initialize aggregate locals before specific initializers
                // so unspecified elements are 0 as required by C.
                if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION ||
                     var->ty->kind == TY_ARRAY) &&
                    var->ty->size > 0) {
                    Node *zinit = new_node(ND_ZERO_INIT, start);
                    zinit->lhs = new_var_node(var, start);
                    cur = cur->next = new_unary(ND_EXPR_STMT, zinit, start);
                }
                tok = local_init_one(tok, lhs, var->ty, &cur);
            }
        }

        if (!equalc(tok, ","))
            break;
        tok = tok->next;
    }

    pending_asm_name = NULL;
    pending_alias_target = NULL;
    *rest = skip(tok, ";");
    return head.next ? head.next : new_node(ND_NULL, tok);
}

static Node *compound_stmt_ex(Token **rest, Token *tok, LVar **out_locals) {
    LVar *saved_locals = locals;
    Typedef *saved_typedefs = typedefs;
    TagScope *saved_tags = tags;
    EnumConst *saved_enum_consts = enum_consts;
    int saved_block_depth = current_block_depth;

    Node head = {};
    Node *cur = &head;
    tok = skip(tok, "{");
    current_block_depth++;

    while (!equalc(tok, "}")) {
        // Handle # pragma pack(N) emitted by the preprocessor
        if (equalc(tok, "#") && equalc(tok->next, "pragma") &&
            equalc(tok->next->next, "pack")) {
            tok = tok->next->next->next;
            if (equalc(tok, "(")) {
                tok = tok->next;
                if (tok->kind == TK_NUM)
                    pack_align = tok->val;
                else
                    pack_align = 0;
                tok = tok->next;
                if (equalc(tok, ")"))
                    tok = tok->next;
            }
            continue;
        }
        // Standalone __attribute__((...)) at statement level (e.g. __fallthrough__)
        if ((equalc(tok, "__attribute__") || equalc(tok, "__attribute"))) {
            Token *after = peek_past_attr(tok);
            if (after && equalc(after, ";")) {
                cur->next = stmt(&tok, tok);
                while (cur->next)
                    cur = cur->next;
                continue;
            }
        }
        if (is_typename(tok)) {
            // A typedef name followed by ':' is a label, not a declaration.
            if (find_typedef(tok) && equalc(tok->next, ":")) {
                cur = cur->next = stmt(&tok, tok);
                continue;
            }
            cur->next = declaration(&tok, tok);
            while (cur->next)
                cur = cur->next;
            continue;
        }
        cur = cur->next = stmt(&tok, tok);
    }

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    *rest = tok->next;

    if (out_locals)
        *out_locals = locals;
    else
        node->body = append_cleanup_range(node->body, locals, saved_locals, tok);
    current_block_depth = saved_block_depth;
    locals = saved_locals;
    typedefs = saved_typedefs;
    tags = saved_tags;
    enum_consts = saved_enum_consts;
    return node;
}

static Node *compound_stmt(Token **rest, Token *tok) {
    return compound_stmt_ex(rest, tok, NULL);
}

static bool is_asm_keyword(Token *tok) {
    return equalc(tok, "asm") || equalc(tok, "__asm__") || equalc(tok, "__asm");
}

#ifdef ARCH_ARM64
// Validate an ARM64 clobber register name.
static bool arm64_is_valid_clobber(const char *s) {
    if (!s || !*s) return false;
    if (strcmp(s, "memory") == 0 || strcmp(s, "cc") == 0) return true;
    // Integer registers: x0-x30, w0-w30, xzr, wzr, sp, wsp
    if ((s[0] == 'x' || s[0] == 'w') && s[1] >= '0' && s[1] <= '9') {
        int n = atoi(s + 1);
        return (n >= 0 && n <= 30) && (s[2] == '\0' || (n >= 10 && s[3] == '\0'));
    }
    if (strcmp(s, "xzr") == 0 || strcmp(s, "wzr") == 0 || strcmp(s, "sp") == 0 || strcmp(s, "wsp") == 0) return true;
    // FP/SIMD: d0-d31, s0-s31, q0-q31, v0-v31, h0-h31, b0-b31
    if ((s[0] == 'd' || s[0] == 's' || s[0] == 'q' || s[0] == 'v' || s[0] == 'h' || s[0] == 'b') &&
        s[1] >= '0' && s[1] <= '9') {
        int n = atoi(s + 1);
        return (n >= 0 && n <= 31);
    }
    return false;
}
#endif

static Node *parse_asm_stmt(Token **rest, Token *tok) {
    Node *node = new_node(ND_ASM, tok);
    tok = tok->next; // skip asm/__asm__/__asm

    // consume optional volatile/goto qualifiers
    while (equalc(tok, "volatile") || equalc(tok, "__volatile__") ||
           equalc(tok, "__volatile") || equalc(tok, "goto"))
        tok = tok->next;

    tok = skip(tok, "(");

    // Concatenate template string literals
    char buf[4096];
    int len = 0;
    while (tok->kind == TK_STR) {
        if (len + tok->len < (int)sizeof(buf) - 1) {
            memcpy(buf + len, tok->str, tok->len);
            len += tok->len;
        }
        tok = tok->next;
    }
    node->asm_template = str_intern(buf, len);

    if (equalc(tok, ")")) {
        *rest = skip(tok->next, ";");
        return node;
    }

    AsmOperand *ops = arena_alloc(sizeof(AsmOperand) * MAX_ASM_OPERANDS);
    for (int i = 0; i < MAX_ASM_OPERANDS; i++) ops[i].reg = -1;
    int nops = 0;

    // Parse output operands
    tok = skip(tok, ":");
    bool first = true;
    while (!equalc(tok, ":") && !equalc(tok, ")")) {
        if (!first) tok = skip(tok, ",");
        first = false;
        if (nops >= MAX_ASM_OPERANDS) error_tok(tok, "too many asm operands");
        AsmOperand *op = &ops[nops++];
        op->name[0] = '\0';
        if (equalc(tok, "[")) { // named operand [id]
            tok = tok->next;
            if (tok->kind == TK_IDENT) {
                int nlen = tok->len < (int)sizeof(op->name) - 1 ? tok->len : (int)sizeof(op->name) - 1;
                memcpy(op->name, tok->loc, nlen);
                op->name[nlen] = '\0';
                tok = tok->next;
            }
            tok = skip(tok, "]");
        }
        if (tok->kind != TK_STR) error_tok(tok, "expected constraint string");
        int clen = tok->len < 15 ? tok->len : 15;
        memcpy(op->constraint, tok->str, clen);
        op->constraint[clen] = '\0';
        tok = tok->next;
        tok = skip(tok, "(");
        op->expr = expr(&tok, tok);
        add_type(op->expr);
        tok = skip(tok, ")");
        for (char *p = op->constraint; *p; p++) {
            if (*p == '=' || *p == '+') op->is_output = true;
            if (*p == '+') op->is_rw = true;
            if (*p == 'm') op->is_memory = true;
        }
    }
    int nout = nops;

    // Parse input operands
    if (!equalc(tok, ")")) {
        tok = skip(tok, ":");
        first = true;
        while (!equalc(tok, ":") && !equalc(tok, ")")) {
            if (!first) tok = skip(tok, ",");
            first = false;
            if (nops >= MAX_ASM_OPERANDS) error_tok(tok, "too many asm operands");
            AsmOperand *op = &ops[nops++];
            op->name[0] = '\0';
            if (equalc(tok, "[")) {
                tok = tok->next;
                if (tok->kind == TK_IDENT) {
                    int nlen = tok->len < (int)sizeof(op->name) - 1 ? tok->len : (int)sizeof(op->name) - 1;
                    memcpy(op->name, tok->loc, nlen);
                    op->name[nlen] = '\0';
                    tok = tok->next;
                }
                tok = skip(tok, "]");
            }
            if (tok->kind != TK_STR) error_tok(tok, "expected constraint string");
            int clen = tok->len < 15 ? tok->len : 15;
            memcpy(op->constraint, tok->str, clen);
            op->constraint[clen] = '\0';
            tok = tok->next;
            tok = skip(tok, "(");
            op->expr = expr(&tok, tok);
            add_type(op->expr);
            tok = skip(tok, ")");
            for (char *p = op->constraint; *p; p++)
                if (*p == 'm') op->is_memory = true;
        }

        // Parse and validate clobbers
        if (!equalc(tok, ")")) {
            tok = skip(tok, ":");
            while (!equalc(tok, ":") && !equalc(tok, ")")) {
                if (tok->kind != TK_STR) error_tok(tok, "expected clobber string");
#ifdef ARCH_ARM64
                if (!arm64_is_valid_clobber(tok->str))
                    error_tok_simple(tok, "invalid clobber register '%s'", tok->str);
#endif
                tok = tok->next;
                if (equalc(tok, ",")) tok = tok->next;
            }

            // Parse goto labels
            if (!equalc(tok, ")")) {
                tok = skip(tok, ":");
                char **glabels = arena_alloc(sizeof(char *) * MAX_ASM_OPERANDS);
                int ngoto = 0;
                first = true;
                while (!equalc(tok, ")")) {
                    if (!first) tok = skip(tok, ",");
                    first = false;
                    if (tok->kind != TK_IDENT) error_tok(tok, "expected label name");
                    glabels[ngoto++] = tok->name;
                    tok = tok->next;
                }
                node->asm_goto_labels = glabels;
                node->asm_ngoto = ngoto;
            }
        }
    }

    node->asm_ops = ops;
    node->asm_nout = nout;
    node->asm_noperands = nops;

#ifdef ARCH_ARM64
    // Validate matching constraint references for extended inline asm
    for (int i = nout; i < nops; i++) {
        const char *c = ops[i].constraint;
        while (*c == '=' || *c == '+' || *c == '&') c++;
        if (*c >= '0' && *c <= '9') {
            int ref = *c - '0';
            if (ref >= nops)
                error_tok_simple(node->tok, "invalid reference in constraint %d ('%c')", i, *c);
        }
    }
#endif

    tok = skip(tok, ")");
    *rest = skip(tok, ";");
    return node;
}

static Node *stmt(Token **rest, Token *tok) {
    if (equalc(tok, "return")) {
        Node *node = new_node(ND_RETURN, tok);
        node->cleanup_begin = locals;
        node->cleanup_end = current_fn_scope_locals;
        if (equalc(tok->next, ";")) {
            *rest = tok->next->next;
            return node;
        }
        node->lhs = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        return node;
    }

    if (equalc(tok, "if")) {
        Node *node = new_node(ND_IF, tok);
        tok = skip(tok->next, "(");
        EnumConst *saved_enum = enum_consts;
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        if (equalc(tok, "else"))
            node->els = stmt(&tok, tok->next);
        enum_consts = saved_enum;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "while")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        EnumConst *saved_enum = enum_consts;
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *saved_loop = current_loop;
        node->cleanup_end = locals;
        node->continue_cleanup_end = locals;
        current_loop = node;
        node->then = stmt(&tok, tok);
        current_loop = saved_loop;
        enum_consts = saved_enum;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "do")) {
        Node *node = new_node(ND_DO, tok);
        Node *saved_loop = current_loop;
        node->cleanup_end = locals;
        node->continue_cleanup_end = locals;
        current_loop = node;
        node->then = stmt(&tok, tok->next);
        current_loop = saved_loop;
        tok = skip(tok, "while");
        tok = skip(tok, "(");
        EnumConst *saved_enum = enum_consts;
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        enum_consts = saved_enum;
        *rest = skip(tok, ";");
        return node;
    }

    if (equalc(tok, "for")) {
        LVar *saved_locals = locals;
        Typedef *saved_typedefs = typedefs;
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        EnumConst *saved_enum = enum_consts;

        if (!equalc(tok, ";")) {
            if (is_typename(tok)) {
                node->init = declaration(&tok, tok);
            } else {
                node->init = expr(&tok, tok);
                tok = skip(tok, ";");
            }
        } else {
            tok = tok->next;
        }

        if (!equalc(tok, ";"))
            node->cond = expr(&tok, tok);
        tok = skip(tok, ";");

        LVar *for_init_locals = locals;

        if (!equalc(tok, ")"))
            node->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *saved_loop = current_loop;
        node->cleanup_end = for_init_locals;
        node->continue_cleanup_end = for_init_locals;
        current_loop = node;
        node->then = stmt(&tok, tok);
        current_loop = saved_loop;
        enum_consts = saved_enum;
        node = append_cleanup_range(node, locals, saved_locals, tok);
        locals = saved_locals;
        typedefs = saved_typedefs;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "switch")) {
        Node *node = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, "(");
        EnumConst *saved_enum = enum_consts;
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *saved = current_switch;
        node->cleanup_end = locals;
        current_switch = node;
        node->then = stmt(&tok, tok);
        current_switch = saved;
        enum_consts = saved_enum;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "case")) {
        if (!current_switch)
            error_tok(tok, "stray case label");
        Node *node = new_node(ND_CASE, tok);
        tok = tok->next;
        Node *val_node = conditional(&tok, tok);
        add_type(val_node);
        long long v = 0;
        if (!eval_const_expr(val_node, &v))
            error_tok(tok, "expected constant expression for case");
        node->case_val = v;
        if (equalc(tok, "...")) {
            tok = tok->next;
            Node *end_node = conditional(&tok, tok);
            add_type(end_node);
            long long ev = 0;
            if (!eval_const_expr(end_node, &ev))
                error_tok(tok, "expected constant expression for case range");
            node->case_end = ev;
            node->is_case_range = true;
        }
        tok = skip(tok, ":");
        node->lhs = stmt(&tok, tok);
        node->case_next = current_switch->case_next;
        current_switch->case_next = node;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "default")) {
        if (!current_switch)
            error_tok(tok, "stray default label");
        Node *node = new_node(ND_CASE, tok);
        node->case_val = -1;
        tok = skip(tok->next, ":");
        node->lhs = stmt(&tok, tok);
        current_switch->default_case = node;
        *rest = tok;
        return node;
    }

    if (equalc(tok, "break")) {
        Node *node = new_node(ND_BREAK, tok);
        node->cleanup_begin = locals;
        *rest = skip(tok->next, ";");
        if (current_switch) {
            node->cleanup_end = current_switch->cleanup_end;
            return node;
        }
        if (current_loop) {
            node->cleanup_end = current_loop->cleanup_end;
            return node;
        }
        error_tok(tok, "stray break");
    }

    if (equalc(tok, "continue")) {
        Node *node = new_node(ND_CONTINUE, tok);
        node->cleanup_begin = locals;
        *rest = skip(tok->next, ";");
        if (!current_loop)
            error_tok(tok, "stray continue");
        node->cleanup_end = current_loop->continue_cleanup_end;
        return node;
    }

    if (equalc(tok, "goto")) {
        tok = tok->next;
        if (equalc(tok, "*")) {
            // Computed goto: goto *expr;
            Node *node = new_node(ND_GOTO_IND, tok);
            tok = tok->next;
            node->lhs = expr(&tok, tok);
            *rest = skip(tok, ";");
            return node;
        }
        Node *node = new_node(ND_GOTO, tok);
        if (tok->kind != TK_IDENT)
            error_tok(tok, "expected label name");
        node->label_name = tok->name;
        node->cleanup_begin = locals;
        LabelScope *label = find_label_scope(node->label_name);
        node->cleanup_end = label ? label->locals : current_fn_scope_locals;
        if (!label)
            add_pending_goto(node->label_name, node);
        *rest = skip(tok->next, ";");
        return node;
    }

    if (tok->kind == TK_IDENT && equalc(tok->next, ":")) {
        Node *node = new_node(ND_LABEL, tok);
        node->label_name = tok->name;
        record_label_scope(node->label_name, locals);
        resolve_pending_gotos(node->label_name, locals);
        tok = tok->next->next;
        tok = skip_attributes(tok);
        node->lhs = stmt(&tok, tok);
        *rest = tok;
        return node;
    }

    if (equalc(tok, "{"))
        return compound_stmt(rest, tok);

    if (equalc(tok, ";")) {
        *rest = tok->next;
        return new_node(ND_NULL, tok);
    }

    if (is_asm_keyword(tok))
        return parse_asm_stmt(rest, tok);

    // Standalone __attribute__((...)) statement (e.g., __fallthrough__)
    if (equalc(tok, "__attribute__") || equalc(tok, "__attribute")) {
        tok = skip_attributes(tok);
        *rest = skip(tok, ";");
        return new_node(ND_NULL, tok);
    }

    Node *node = new_node(ND_EXPR_STMT, tok);
    node->lhs = expr(&tok, tok);
    *rest = skip(tok, ";");
    return node;
}

static bool type_equal(Type *a, Type *b) {
    if (a == b)
        return true;
    if (!a || !b)
        return false;
    if (a->kind != b->kind)
        return false;
    if (a->qual != b->qual)
        return false;
    if (a->is_unsigned != b->is_unsigned)
        return false;
    if (a->kind == TY_CHAR && a->is_signed_char != b->is_signed_char)
        return false;
    if (a->is_variadic != b->is_variadic)
        return false;

    switch (a->kind) {
    case TY_PTR:
        return type_equal(a->base, b->base);
    case TY_ARRAY:
        if (a->size != b->size)
            return false;
        return type_equal(a->base, b->base);
    case TY_FUNC:
        if (!type_equal(a->return_ty, b->return_ty))
            return false;
        {
            Type *pa = a->param_types;
            Type *pb = b->param_types;
            // If either side lacks parameter info (common for typedefs/fwd-decls),
            // consider them compatible as long as return types match.
            if (!pa || !pb)
                return true;
            while (pa && pb) {
                if (!type_equal(pa, pb))
                    return false;
                pa = pa->param_next;
                pb = pb->param_next;
            }
            return !pa && !pb;
        }
    case TY_STRUCT:
    case TY_UNION:
        return a == b;
    default:
        return true;
    }
}

static Node *primary(Token **rest, Token *tok) {
    Node *node = NULL;

    if (equalc(tok, "_Generic")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ctrl = assign(&tok, tok);
        add_type(ctrl);
        Type *ctrl_ty = ctrl->ty;
        // Apply lvalue/array/function decay
        if (ctrl_ty->kind == TY_ARRAY)
            ctrl_ty = pointer_to(ctrl_ty->base);
        else if (ctrl_ty->kind == TY_FUNC)
            ctrl_ty = pointer_to(ctrl_ty);
        // Lvalue conversion strips top-level qualifiers
        if (ctrl_ty->qual) {
            ctrl_ty = copy_type(ctrl_ty);
            ctrl_ty->qual = 0;
        }

        tok = skip(tok, ",");

        Node *selected = NULL;
        Node *default_expr = NULL;
        while (!equalc(tok, ")")) {
            if (equalc(tok, "default")) {
                tok = skip(tok->next, ":");
                default_expr = assign(&tok, tok);
            } else {
                Type *ty = type_name(&tok, tok);
                tok = skip(tok, ":");
                Node *expr = assign(&tok, tok);
                if (type_equal(ctrl_ty, ty))
                    selected = expr;
            }
            if (equalc(tok, ","))
                tok = tok->next;
        }

        if (!selected && default_expr)
            selected = default_expr;
        if (!selected)
            error_tok(start, "_Generic: no matching association");

        tok = skip(tok, ")");
        node = selected;
    } else if (equalc(tok, "(")) {
        if (equalc(tok->next, "{")) {
            node = new_node(ND_STMT_EXPR, tok);
            LVar *block_locals = NULL;
            Node *block = compound_stmt_ex(&tok, tok->next, &block_locals);
            node->body = block->body;
            // Find result BEFORE cleanup nodes are appended
            Node *last = node->body;
            while (last && last->next)
                last = last->next;
            if (last && last->kind == ND_EXPR_STMT && last->lhs)
                node->stmt_expr_result = last->lhs;
            // Append cleanups as a flat list (block_locals → locals = saved after block)
            node->body = append_cleanup_flat(node->body, block_locals, locals, tok);
            tok = skip(tok, ")");
        } else {
            node = expr(&tok, tok->next);
            tok = skip(tok, ")");
        }
    } else if (tok->kind == TK_IDENT) {
        // __FUNCTION__, __func__, __PRETTY_FUNCTION__ → current function name string
        if (equalc(tok, "__FUNCTION__") || equalc(tok, "__func__") || equalc(tok, "__PRETTY_FUNCTION__")) {
            const char *fn = parser_current_fn ? parser_current_fn : "";
            node = new_node(ND_STR, tok);
            node->ty = array_of(ty_char, strlen(fn) + 1);
            StrLit *s = new_str_lit((char *)fn, strlen(fn), 0, 1);
            node->str_id = s->id;
            *rest = tok->next;
            return node;
        }
        if (equalc(tok->next, "(")) {
            node = new_node(ND_FUNCALL, tok);
            LVar *var = find_var(tok);
            if (var)
                node->lhs = new_var_node(var, tok);
            else {
                node->funcname = tok->name;
                LVar *gvar = find_global_name(tok->name);
                if (gvar && gvar->is_function)
                    node->lhs = new_var_node(gvar, tok);
            }
            tok = skip(tok->next, "(");
            Node head = {};
            Node *cur = &head;
            while (!equalc(tok, ")")) {
                if (cur != &head)
                    tok = skip(tok, ",");
                cur = cur->next = assign(&tok, tok);
            }
            node->args = head.next;
            tok = skip(tok, ")");
            cast_funcall_args(node);
        } else {
            EnumConst *ec = find_enum_const(tok);
            if (ec) {
                node = new_num(ec->val, tok);
                tok = tok->next;
            } else if (equalc(tok, "NULL")) {
                node = new_num(0, tok);
                tok = tok->next;
            } else {
                LVar *var = find_var(tok);
                if (!var)
                    error_tok(tok, "undefined variable\n\033[1;36mnote\033[0m: variable must be declared before use (e.g., 'int x = 0;')");
                node = new_var_node(var, tok);
                tok = tok->next;
            }
        }
    } else if (equalc(tok, "&&") && tok->next && tok->next->kind == TK_IDENT) {
        // GCC label address: &&label
        node = new_node(ND_LABEL_VAL, tok);
        node->label_name = tok->next->name;
        node->ty = pointer_to(ty_void);
        tok = tok->next->next;
    } else if (tok->kind == TK_NUM) {
        node = new_num(tok->val, tok);
        tok = tok->next;
    } else if (tok->kind == TK_FNUM) {
        node = new_fnum(tok->fval, tok);
        if (tok->val == 1)
            node->ty = ty_float;
        tok = tok->next;
    } else if (tok->kind == TK_STR) {
        node = new_node(ND_STR, tok);
        node->str = tok->str;
        // Set the type based on the string literal prefix
        switch (tok->string_literal_prefix) {
        case 0: // Regular string
            node->ty = pointer_to(ty_char);
            break;
        case 'L': // Wide string
#ifdef _WIN32
            node->ty = pointer_to(ty_ushort);
#else
            node->ty = pointer_to(ty_uint);
#endif
            break;
        case 'u': // char16_t string
        {
            Type *char16_t_type = typedef_find_name("char16_t");
            if (!char16_t_type) {
                // Fallback to unsigned short if not defined
                char16_t_type = ty_ushort;
            }
            node->ty = pointer_to(char16_t_type);
        } break;
        case 'U': // char32_t string
        {
            Type *char32_t_type = typedef_find_name("char32_t");
            if (!char32_t_type) {
                // Fallback to unsigned int if not defined
                char32_t_type = ty_uint;
            }
            node->ty = pointer_to(char32_t_type);
        } break;
        default: // Fallback to regular string
            node->ty = pointer_to(ty_char);
            break;
        }
        StrLit *s = new_str_lit(tok->str, tok->len, tok->string_literal_prefix, node->ty->base->size);
        node->str_id = s->id;
        tok = tok->next;
    } else {
        error_tok(tok, "expected an expression");
    }

    add_type(node);

    while (true) {
        if (equalc(tok, "(")) {
            Node *call = new_node(ND_FUNCALL, tok);
            call->lhs = node;
            tok = tok->next;
            Node head = {};
            Node *cur = &head;
            while (!equalc(tok, ")")) {
                if (cur != &head)
                    tok = skip(tok, ",");
                cur = cur->next = assign(&tok, tok);
            }
            call->args = head.next;
            tok = skip(tok, ")");
            cast_funcall_args(call);
            node = call;
            add_type(node);
            continue;
        }
        if (equalc(tok, "[")) {
            Token *start = tok;
            Node *idx = expr(&tok, tok->next);
            tok = skip(tok, "]");
            node = new_unary(ND_DEREF, new_binary(ND_ADD, node, idx, start), start);
            add_type(node);
            continue;
        }
        if (equalc(tok, ".")) {
            tok = tok->next;
            add_type(node);
            Member *mem = find_member(node->ty, tok);
            if (!mem)
                error_tok(tok, "no such member");
            Node *mem_node = new_unary(ND_MEMBER, node, tok);
            mem_node->member = mem;
            if (mem->bit_width > 0) {
                int bw = mem->bit_width;
                if (bw < 32 || (bw == 32 && !mem->ty->is_unsigned))
                    mem_node->ty = ty_int;
                else if (bw == 32)
                    mem_node->ty = ty_uint;
                else
                    mem_node->ty = mem->ty;
            } else {
                mem_node->ty = mem->ty;
            }
            node = mem_node;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "->")) {
            tok = tok->next;
            add_type(node);
            if ((node->ty->kind != TY_PTR && node->ty->kind != TY_ARRAY) ||
                (node->ty->base->kind != TY_STRUCT && node->ty->base->kind != TY_UNION))
                error_tok(tok, "not a pointer to struct or union");
            node = new_unary(ND_DEREF, node, tok);
            add_type(node);
            Member *mem = find_member(node->ty, tok);
            if (!mem)
                error_tok(tok, "no such member");
            Node *mem_node = new_unary(ND_MEMBER, node, tok);
            mem_node->member = mem;
            if (mem->bit_width > 0) {
                int bw = mem->bit_width;
                if (bw < 32 || (bw == 32 && !mem->ty->is_unsigned))
                    mem_node->ty = ty_int;
                else if (bw == 32)
                    mem_node->ty = ty_uint;
                else
                    mem_node->ty = mem->ty;
            } else {
                mem_node->ty = mem->ty;
            }
            node = mem_node;
            tok = tok->next;
            continue;
        }
        if (equalc(tok, "++")) {
            node = new_unary(ND_POST_INC, node, tok);
            tok = tok->next;
            add_type(node);
            continue;
        }
        if (equalc(tok, "--")) {
            node = new_unary(ND_POST_DEC, node, tok);
            tok = tok->next;
            add_type(node);
            continue;
        }
        break;
    }

    *rest = tok;
    return node;
}

static int parse_memory_order(Token **rest) {
    Token *tok = *rest;
    if (tok->kind == TK_NUM) {
        *rest = tok->next;
        return (int)tok->val;
    }
    EnumConst *ec = find_enum_const(tok);
    if (ec) {
        *rest = tok->next;
        return (int)ec->val;
    }
    Node *node = assign(rest, tok);
    add_type(node);
    if (node->kind == ND_NUM)
        return (int)node->val;
    return MEMORDER_SEQ_CST;
}


static Node *unary(Token **rest, Token *tok) {
    if (equalc(tok, "__builtin_offsetof")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Type *ty = type_name(&tok, tok);
        tok = skip(tok, ",");

        int offset = 0;
        while (true) {
            if (tok->kind != TK_IDENT)
                error_tok(tok, "expected member name");
            Member *mem = find_member(ty, tok);
            if (!mem)
                error_tok(tok, "no such member");
            offset += mem->offset;
            ty = mem->ty;
            tok = tok->next;

            if (equalc(tok, "[")) {
                tok = tok->next;
                if (tok->kind != TK_NUM || ty->kind != TY_ARRAY)
                    error_tok(tok, "unsupported offsetof designator");
                offset += tok->val * ty->base->size;
                ty = ty->base;
                tok = skip(tok->next, "]");
            }

            if (!equalc(tok, "."))
                break;
            tok = tok->next;
        }

        *rest = skip(tok, ")");
        return new_num(offset, start);
    }
    if (equalc(tok, "__builtin_va_start")) {
        Node *node = new_node(ND_VA_START, tok);
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        tok = skip(tok, ",");
        assign(&tok, tok);
        *rest = skip(tok, ")");
        return node;
    }
    if (equalc(tok, "__builtin_va_copy")) {
        Node *node = new_node(ND_VA_COPY, tok);
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        tok = skip(tok, ",");
        node->rhs = assign(&tok, tok);
        *rest = skip(tok, ")");
        return node;
    }
    if (equalc(tok, "__builtin_va_end")) {
        tok = skip(tok->next, "(");
        Node *node = assign(&tok, tok);
        *rest = skip(tok, ")");
        return node;
    }
    if (equalc(tok, "__builtin_va_arg")) {
        Node *node = new_node(ND_VA_ARG, tok);
        tok = skip(tok->next, "(");

        Node *ap_arg = assign(&tok, tok);
        add_type(ap_arg);
        node->lhs = ap_arg;
        tok = skip(tok, ",");

        VarAttr attr = {0};
        Type *ty = type_name(&tok, tok);
        (void)attr;
        *rest = skip(tok, ")");

        node->ty = pointer_to(ty);
        node = new_unary(ND_DEREF, node, tok);
        return node;
    }
    if (equalc(tok, "__atomic_is_lock_free")) {
        tok = skip(tok->next, "(");
        assign(&tok, tok);
        tok = skip(tok, ",");
        assign(&tok, tok);
        *rest = skip(tok, ")");
        return new_num(1, tok);
    }
    if (equalc(tok, "__atomic_thread_fence")) {
        Node *node = new_node(ND_ATOMIC_FENCE, tok);
        node->atomic_ord = MEMORDER_SEQ_CST;
        tok = skip(tok->next, "(");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        return node;
    }
    if (equalc(tok, "__atomic_signal_fence")) {
        Node *node = new_node(ND_ATOMIC_FENCE, tok);
        node->atomic_signal_fence = true;
        node->atomic_ord = MEMORDER_SEQ_CST;
        tok = skip(tok->next, "(");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        return node;
    }
    if (equalc(tok, "__atomic_test_and_set")) {
        Token *start = tok;
        Node *node = new_node(ND_ATOMIC_EXCHANGE, start);
        node->atomic_ord = MEMORDER_SEQ_CST;
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        add_type(node->lhs);
        if (equalc(tok, ",")) {
            tok = tok->next;
            node->atomic_ord = parse_memory_order(&tok);
        }
        *rest = skip(tok, ")");
        node->rhs = new_num(1, start);
        node->ty = ty_bool;
        return node;
    }
    if (equalc(tok, "__atomic_clear")) {
        Token *start = tok;
        Node *node = new_node(ND_ATOMIC_STORE, start);
        node->atomic_ord = MEMORDER_SEQ_CST;
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        add_type(node->lhs);
        if (equalc(tok, ",")) {
            tok = tok->next;
            node->atomic_ord = parse_memory_order(&tok);
        }
        *rest = skip(tok, ")");
        node->rhs = new_num(0, start);
        node->ty = ty_void;
        return node;
    }
    if (equalc(tok, "__atomic_load_n") || equalc(tok, "__atomic_load")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        if (ptr->ty->kind != TY_PTR && ptr->ty->kind != TY_ARRAY)
            error_tok(start, "pointer expected");
        else {
            Type *base = ptr->ty->base;
            if (!base || base->size == 0 || base->size > 8 || (base->size & (base->size - 1)))
                error_tok(start, "integral or integer-sized pointer target type expected");
        }
        tok = skip(tok, ",");
        Node *node = new_node(ND_ATOMIC_LOAD, start);
        node->lhs = ptr;
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        if (ptr->ty->base)
            node->ty = ptr->ty->base;
        else
            node->ty = ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_store_n") || equalc(tok, "__atomic_store")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        if (ptr->ty->kind == TY_PTR || ptr->ty->kind == TY_ARRAY) {
            Type *base = ptr->ty->base;
            if (base) {
                if (ty_const(base))
                    warn_tok(start, "assignment of read-only location");
                if (equalc(start, "__atomic_store_n")) {
                    if (!val->ty) {
                    } else if (is_integer(base) && (val->ty->kind == TY_PTR || val->ty->kind == TY_ARRAY))
                        warn_tok(start, "assignment makes integer from pointer without a cast");
                    else if (base->kind == TY_PTR && (val->ty->kind == TY_PTR || val->ty->kind == TY_ARRAY)) {
                        Type *bbase = base->base, *vbase = val->ty->base;
                        if (bbase && vbase && bbase->kind != TY_VOID && vbase->kind != TY_VOID &&
                            (bbase->kind != vbase->kind || bbase->size != vbase->size))
                            warn_tok(start, "assignment from incompatible pointer type");
                    }
                }
            }
        }
        Node *node = new_node(ND_ATOMIC_STORE, start);
        node->lhs = ptr;
        node->rhs = val;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ty_void;
        return node;
    }
    if (equalc(tok, "__atomic_exchange_n") || equalc(tok, "__atomic_exchange")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        if (ptr->ty->kind == TY_PTR || ptr->ty->kind == TY_ARRAY) {
            Type *base = ptr->ty->base;
            if (base && ty_const(base))
                warn_tok(start, "assignment of read-only location");
        }
        Node *node = new_node(ND_ATOMIC_EXCHANGE, start);
        node->lhs = ptr;
        node->rhs = val;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        if (ptr->ty->base)
            node->ty = ptr->ty->base;
        else
            node->ty = ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_compare_exchange_n") || equalc(tok, "__atomic_compare_exchange")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *expected = assign(&tok, tok);
        add_type(expected);
        tok = skip(tok, ",");
        Node *desired = assign(&tok, tok);
        add_type(desired);
        if ((ptr->ty->kind == TY_PTR || ptr->ty->kind == TY_ARRAY) && ptr->ty->base) {
            Type *base = ptr->ty->base;
            if ((expected->ty->kind == TY_PTR || expected->ty->kind == TY_ARRAY) && expected->ty->base) {
                if (expected->ty->base->size != base->size)
                    error_tok(start, "pointer target type mismatch in argument 2");
            } else {
                error_tok(start, "pointer target type mismatch in argument 2");
            }
        }
        Node *node = new_node(ND_ATOMIC_CAS, start);
        node->lhs = ptr;
        node->body = expected;
        node->rhs = desired;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_ord2 = MEMORDER_SEQ_CST;
        node->atomic_weak = false;
        if (equalc(tok, ",")) {
            tok = tok->next;
            node->atomic_weak = !!parse_memory_order(&tok);
            if (equalc(tok, ",")) {
                tok = tok->next;
                node->atomic_ord = parse_memory_order(&tok);
                if (equalc(tok, ",")) {
                    tok = tok->next;
                    node->atomic_ord2 = parse_memory_order(&tok);
                }
            }
        }
        *rest = skip(tok, ")");
        node->ty = ty_bool;
        return node;
    }
#define ATOMIC_FETCH_OP_HELPER(name_str, op_val) do { \
    Token *start = tok; \
    tok = skip(tok->next, "("); \
    Node *ptr = assign(&tok, tok); \
    add_type(ptr); \
    tok = skip(tok, ","); \
    Node *val = assign(&tok, tok); \
    add_type(val); \
    if (ptr->ty->kind == TY_PTR || ptr->ty->kind == TY_ARRAY) { \
        Type *base = ptr->ty->base; \
        if (!base || base->kind == TY_PTR || base->size == 0 || base->size > 8 || (base->size & (base->size - 1))) \
            error_tok(start, "integral or integer-sized pointer target type expected"); \
        else if (ty_const(base)) \
            warn_tok(start, "assignment of read-only location"); \
    } \
    Node *node = new_node(ND_ATOMIC_FETCH_OP, start); \
    node->lhs = ptr; \
    node->rhs = val; \
    node->atomic_fetch_op = (op_val); \
    tok = skip(tok, ","); \
    node->atomic_ord = parse_memory_order(&tok); \
    *rest = skip(tok, ")"); \
    if (ptr->ty->base) \
        node->ty = ptr->ty->base; \
    else \
        node->ty = ty_int; \
    return node; \
} while(0)
    if (equalc(tok, "__atomic_fetch_add"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_add", 0);
    if (equalc(tok, "__atomic_fetch_sub"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_sub", 1);
    if (equalc(tok, "__atomic_fetch_or"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_or", 2);
    if (equalc(tok, "__atomic_fetch_xor"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_xor", 3);
    if (equalc(tok, "__atomic_fetch_and"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_and", 4);
    if (equalc(tok, "__atomic_fetch_nand"))
        ATOMIC_FETCH_OP_HELPER("__atomic_fetch_nand", 5);
#undef ATOMIC_FETCH_OP_HELPER
    if (equalc(tok, "__atomic_add_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 0;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_sub_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 1;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_or_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 2;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_xor_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 3;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_and_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 4;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__atomic_nand_fetch")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 5;
        node->atomic_is_store = true;
        tok = skip(tok, ",");
        node->atomic_ord = parse_memory_order(&tok);
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_synchronize")) {
        Node *node = new_node(ND_ATOMIC_FENCE, tok);
        node->body = NULL;
        node->atomic_ord = MEMORDER_SEQ_CST;
        *rest = skip(tok, "(");
        *rest = skip(*rest, ")");
        return node;
    }
    if (equalc(tok, "__sync_lock_test_and_set")) {
        Token *start = tok;
        Node *node = new_node(ND_ATOMIC_EXCHANGE, start);
        node->atomic_ord = MEMORDER_ACQ_REL;
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        tok = skip(tok, ",");
        node->rhs = assign(&tok, tok);
        add_type(node->rhs);
        *rest = skip(tok, ")");
        if (node->lhs->ty && node->lhs->ty->base)
            node->ty = node->lhs->ty->base;
        else
            node->ty = ty_int;
        return node;
    }
    if (equalc(tok, "__sync_lock_release")) {
        Token *start = tok;
        Node *node = new_node(ND_ATOMIC_STORE, start);
        node->atomic_ord = MEMORDER_RELEASE;
        tok = skip(tok->next, "(");
        node->lhs = assign(&tok, tok);
        node->rhs = new_num(0, start);
        *rest = skip(tok, ")");
        node->ty = ty_void;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_add")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 0;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_sub")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 1;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_or")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 2;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_xor")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 3;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_and")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 4;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_fetch_and_nand")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *val = assign(&tok, tok);
        add_type(val);
        Node *node = new_node(ND_ATOMIC_FETCH_OP, start);
        node->lhs = ptr;
        node->rhs = val;
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_fetch_op = 5;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_val_compare_and_swap")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *oldval = assign(&tok, tok);
        add_type(oldval);
        tok = skip(tok, ",");
        Node *newval = assign(&tok, tok);
        add_type(newval);
        Node *node = new_node(ND_ATOMIC_CAS, start);
        node->lhs = ptr;
        node->rhs = newval;
        node->body = new_unary(ND_ADDR, oldval, start);
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_ord2 = MEMORDER_SEQ_CST;
        *rest = skip(tok, ")");
        node->ty = ptr->ty->base ? ptr->ty->base : ty_int;
        return node;
    }
    if (equalc(tok, "__sync_bool_compare_and_swap")) {
        Token *start = tok;
        tok = skip(tok->next, "(");
        Node *ptr = assign(&tok, tok);
        add_type(ptr);
        tok = skip(tok, ",");
        Node *oldval = assign(&tok, tok);
        add_type(oldval);
        tok = skip(tok, ",");
        Node *newval = assign(&tok, tok);
        add_type(newval);
        Node *node = new_node(ND_ATOMIC_CAS, start);
        node->lhs = ptr;
        node->rhs = newval;
        node->body = new_unary(ND_ADDR, oldval, start);
        node->atomic_ord = MEMORDER_SEQ_CST;
        node->atomic_ord2 = MEMORDER_SEQ_CST;
        *rest = skip(tok, ")");
        node->ty = ty_bool;
        return node;
    }
    if (equalc(tok, "++")) {
        Token *start = tok;
        Node *lhs = unary(&tok, tok->next);
        *rest = tok;
        return new_binary(ND_ASSIGN, lhs, new_binary(ND_ADD, lhs, new_num(1, start), start), start);
    }
    if (equalc(tok, "--")) {
        Token *start = tok;
        Node *lhs = unary(&tok, tok->next);
        *rest = tok;
        return new_binary(ND_ASSIGN, lhs, new_binary(ND_SUB, lhs, new_num(1, start), start), start);
    }
    if (equalc(tok, "+"))
        return unary(rest, tok->next);
    if (equalc(tok, "-"))
        return new_unary(ND_NEG, unary(rest, tok->next), tok);
    if (equalc(tok, "!"))
        return new_unary(ND_NOT, unary(rest, tok->next), tok);
    if (equalc(tok, "~"))
        return new_unary(ND_BITNOT, unary(rest, tok->next), tok);
    if (equalc(tok, "&"))
        return new_unary(ND_ADDR, unary(rest, tok->next), tok);
    if (equalc(tok, "*"))
        return new_unary(ND_DEREF, unary(rest, tok->next), tok);
    if (equalc(tok, "sizeof")) {
        if (equalc(tok->next, "(") && is_typename(tok->next->next)) {
            Type *ty = parse_cast_type(&tok, tok->next);
            *rest = tok;
            return new_num(ty->size, tok);
        }
        Node *node = unary(&tok, tok->next);
        add_type(node);
        *rest = tok;
        return new_num(node->ty->size, tok);
    }
    if (equalc(tok, "__alignof__") || equalc(tok, "__alignof") || equalc(tok, "_Alignof")) {
        Token *start = tok;
        if (equalc(tok->next, "(") && is_typename(tok->next->next)) {
            Type *ty = parse_cast_type(&tok, tok->next);
            *rest = tok;
            return new_num(ty->align, start);
        }
        Node *node = unary(&tok, tok->next);
        add_type(node);
        *rest = tok;
        return new_num(node->ty->align, start);
    }
    if (is_cast(tok)) {
        Token *start = tok;
        Type *ty = parse_cast_type(&tok, tok);

        // Compound literal: (type){init_list}
        if (equalc(tok, "{")) {
            tok = tok->next;

            // For incomplete arrays, count elements first
            if (ty->kind == TY_ARRAY && ty->size == 0 && ty->base) {
                Token *tmp = tok;
                int count = 0;
                int depth = 0;
                while (true) {
                    if (equalc(tmp, "{")) depth++;
                    else if (equalc(tmp, "}")) {
                        if (depth == 0) break;
                        depth--;
                    }
                    if (depth == 0 && (equalc(tmp, ",") || equalc(tmp, "}")))
                        ;
                    else
                        count++;
                    // Advance past comma-separated items
                    if (depth == 0 && equalc(tmp->next, ",")) {
                        tmp = tmp->next->next;
                        continue;
                    }
                    if (depth == 0 && equalc(tmp->next, "}")) {
                        tmp = tmp->next;
                        continue;
                    }
                    tmp = tmp->next;
                }
                // Simple count: count commas + 1
                tmp = tok;
                count = 1;
                depth = 0;
                while (!(depth == 0 && equalc(tmp, "}"))) {
                    if (equalc(tmp, "{")) depth++;
                    else if (equalc(tmp, "}"))
                        depth--;
                    else if (depth == 0 && equalc(tmp, ","))
                        count++;
                    tmp = tmp->next;
                }
                // Handle trailing comma
                Token *before_end = tok;
                for (Token *t = tok; !equalc(t, "}"); t = t->next)
                    before_end = t;
                if (equalc(before_end, ","))
                    count--;
                ty = array_of(ty->base, count);
            }

            static int anon_count;
            char *name = format(".Lanon.%d", anon_count++);
            LVar *var = new_var(name, ty, true);

            Node *result = new_var_node(var, start);

            if (ty->kind == TY_ARRAY && ty->base) {
                // Array compound literal: assign each element
                int i = 0;
                while (!equalc(tok, "}")) {
                    Node *idx = new_num(i, start);
                    Node *elem_ptr = new_binary(ND_ADD, new_var_node(var, start), idx, start);
                    Node *deref = new_unary(ND_DEREF, elem_ptr, start);
                    Node *val = assign(&tok, tok);
                    add_type(val);
                    Node *asgn = new_binary(ND_ASSIGN, deref, val, start);
                    add_type(asgn);
                    result = new_binary(ND_COMMA, result, asgn, start);
                    add_type(result);
                    i++;
                    if (!equalc(tok, "}"))
                        tok = skip(tok, ",");
                }
                tok = tok->next; // skip }
                // Final value is the array (decays to pointer)
                // Re-wrap so the last value is the variable itself
                Node *final_var = new_var_node(var, start);
                result = new_binary(ND_COMMA, result, final_var, start);
                add_type(result);
                // Compound literals are lvalues; preserve the array type
                // so sizeof and other operations see the correct size.
                result->ty = var->ty;
            } else if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
                // Struct compound literal: assign each member
                Member *mem = ty->members;
                while (!equalc(tok, "}") && mem) {
                    if (mem->ty->kind == TY_ARRAY) {
                        // Array member: assign elements, handle optional braces
                        int len = array_len(mem->ty);
                        int idx = 0;
                        bool arr_brace = equalc(tok, "{");
                        if (arr_brace) tok = tok->next;
                        while (idx < len && !equalc(tok, "}")) {
                            int sidx = idx, eidx = idx;
                            // Designated initializer: [N] = val or [N ... M] = val
                            if (equalc(tok, "[")) {
                                tok = tok->next;
                                Node *n = assign(&tok, tok);
                                long long sv = 0;
                                eval_const_expr(n, &sv);
                                sidx = (int)sv;
                                eidx = sidx;
                                if (equalc(tok, "...")) {
                                    tok = tok->next;
                                    Node *n2 = assign(&tok, tok);
                                    long long ev = sidx;
                                    eval_const_expr(n2, &ev);
                                    eidx = (int)ev;
                                }
                                tok = skip(tok, "]");
                                tok = skip(tok, "=");
                                idx = sidx;
                            }
                            for (int i = sidx; i <= eidx; i++) {
                                if (len == 0 || i < len) {
                                    Node *var_node = new_var_node(var, start);
                                    Node *member_access = new_node(ND_MEMBER, start);
                                    member_access->lhs = var_node;
                                    member_access->member = mem;
                                    member_access->ty = mem->ty;
                                    Node *offset = new_num(i, start);
                                    Node *elem_ptr = new_binary(ND_ADD, member_access, offset, start);
                                    Node *elem_lhs = new_unary(ND_DEREF, elem_ptr, start);
                                    Token *val_start = tok;
                                    Node *val = assign(&tok, tok);
                                    add_type(val);
                                    Node *asgn = new_binary(ND_ASSIGN, elem_lhs, val, start);
                                    add_type(asgn);
                                    result = new_binary(ND_COMMA, result, asgn, start);
                                    result->ty = ty;
                                    // Reset tok for ranged initializer re-evaluation
                                    if (i < eidx)
                                        tok = val_start;
                                }
                            }
                            idx = eidx + 1;
                            if (equalc(tok, ",")) {
                                tok = tok->next;
                                if (equalc(tok, "}"))
                                    break;
                                continue;
                            }
                            break;
                        }
                        if (arr_brace) tok = skip(tok, "}");
                    } else if ((mem->ty->kind == TY_STRUCT || mem->ty->kind == TY_UNION) && equalc(tok, "{")) {
                        // Struct/union member with brace-enclosed initializer
                        tok = skip(tok, "{");
                        Member *sub = mem->ty->members;
                        while (!equalc(tok, "}")) {
                            Node *var_node = new_var_node(var, start);
                            Node *member_access = new_node(ND_MEMBER, start);
                            member_access->lhs = var_node;
                            member_access->member = mem;
                            member_access->ty = mem->ty;
                            // Designated initializer: .name = value
                            if (equalc(tok, ".") && tok->next && tok->next->kind == TK_IDENT) {
                                char *name = tok->next->name;
                                tok = tok->next->next;
                                tok = skip(tok, "=");
                                Member *m = find_member_by_name(mem->ty, name);
                                if (m) {
                                    Node *inner_access = new_node(ND_MEMBER, start);
                                    inner_access->lhs = member_access;
                                    inner_access->member = m;
                                    inner_access->ty = m->ty;
                                    Node *val = assign(&tok, tok);
                                    add_type(val);
                                    Node *asgn = new_binary(ND_ASSIGN, inner_access, val, start);
                                    asgn->ty = m->ty;
                                    result = new_binary(ND_COMMA, result, asgn, start);
                                    result->ty = ty;
                                } else {
                                    tok = skip_initializer(tok);
                                }
                            } else if (sub) {
                                Node *inner_access = new_node(ND_MEMBER, start);
                                inner_access->lhs = member_access;
                                inner_access->member = sub;
                                inner_access->ty = sub->ty;
                                Node *val = assign(&tok, tok);
                                add_type(val);
                                Node *asgn = new_binary(ND_ASSIGN, inner_access, val, start);
                                asgn->ty = sub->ty;
                                result = new_binary(ND_COMMA, result, asgn, start);
                                result->ty = ty;
                                sub = sub->next;
                            } else {
                                tok = skip_initializer(tok);
                            }
                            if (equalc(tok, ",")) {
                                tok = tok->next;
                                if (equalc(tok, "}"))
                                    break;
                                continue;
                            }
                            break;
                        }
                        tok = skip(tok, "}");
                    } else {
                        Node *var_node = new_var_node(var, start);
                        Node *member_access = new_node(ND_MEMBER, start);
                        member_access->lhs = var_node;
                        member_access->member = mem;
                        member_access->ty = mem->ty;
                        Node *val = assign(&tok, tok);
                        add_type(val);
                        Node *asgn = new_binary(ND_ASSIGN, member_access, val, start);
                        asgn->ty = mem->ty;
                        result = new_binary(ND_COMMA, result, asgn, start);
                        result->ty = ty;
                    }
                    mem = mem->next;
                    if (!equalc(tok, "}"))
                        tok = skip(tok, ",");
                }
                while (!equalc(tok, "}")) {
                    // Skip extra initializers
                    if (equalc(tok, ",")) {
                        tok = tok->next;
                        continue;
                    }
                    assign(&tok, tok);
                    if (!equalc(tok, "}"))
                        tok = skip(tok, ",");
                }
                tok = tok->next; // skip }
                Node *final_var = new_var_node(var, start);
                result = new_binary(ND_COMMA, result, final_var, start);
                add_type(result);
            } else {
                // Scalar compound literal
                Node *val = assign(&tok, tok);
                add_type(val);
                if (equalc(tok, ",")) tok = tok->next;
                tok = skip(tok, "}");
                Node *asgn = new_binary(ND_ASSIGN, new_var_node(var, start), val, start);
                asgn->ty = ty;
                result = new_binary(ND_COMMA, asgn, new_var_node(var, start), start);
                add_type(result);
            }

            *rest = tok;
            return result;
        }

        Node *lhs = unary(rest, tok);
        add_type(lhs);
        Node *node = new_unary(ND_CAST, lhs, start);
        node->ty = ty;
        return node;
    }
    return primary(rest, tok);
}

static Node *mul(Token **rest, Token *tok) {
    Node *node = unary(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equalc(tok, "*")) {
            node = new_binary(ND_MUL, node, unary(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, "/")) {
            node = new_binary(ND_DIV, node, unary(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, "%")) {
            node = new_binary(ND_MOD, node, unary(&tok, tok->next), start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *add(Token **rest, Token *tok) {
    Node *node = mul(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equalc(tok, "+")) {
            node = new_binary(ND_ADD, node, mul(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, "-")) {
            node = new_binary(ND_SUB, node, mul(&tok, tok->next), start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *shift(Token **rest, Token *tok) {
    Node *node = add(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equalc(tok, "<<")) {
            node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, ">>")) {
            node = new_binary(ND_SHR, node, add(&tok, tok->next), start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *relational(Token **rest, Token *tok) {
    Node *node = shift(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equalc(tok, "<")) {
            node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, "<=")) {
            node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, ">")) {
            node = new_binary(ND_LT, shift(&tok, tok->next), node, start);
            continue;
        }
        if (equalc(tok, ">=")) {
            node = new_binary(ND_LE, shift(&tok, tok->next), node, start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *equality(Token **rest, Token *tok) {
    Node *node = relational(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equalc(tok, "==")) {
            node = new_binary(ND_EQ, node, relational(&tok, tok->next), start);
            continue;
        }
        if (equalc(tok, "!=")) {
            node = new_binary(ND_NE, node, relational(&tok, tok->next), start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *bitand(Token **rest, Token *tok) {
    Node *node = equality(&tok, tok);
    while (equalc(tok, "&")) {
        Token *start = tok;
        node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitxor(Token **rest, Token *tok) {
    Node *node = bitand(&tok, tok);
    while (equalc(tok, "^")) {
        Token *start = tok;
        node = new_binary(ND_BITXOR, node, bitand(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitor(Token **rest, Token *tok) {
    Node *node = bitxor(&tok, tok);
    while (equalc(tok, "|")) {
        Token *start = tok;
        node = new_binary(ND_BITOR, node, bitxor(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *logand(Token **rest, Token *tok) {
    Node *node = bitor(&tok, tok);
    while (equalc(tok, "&&"))
        node = new_binary(ND_LOGAND, node, bitor(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static Node *logor(Token **rest, Token *tok) {
    Node *node = logand(&tok, tok);
    while (equalc(tok, "||"))
        node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static Node *conditional(Token **rest, Token *tok) {
    Node *node = logor(&tok, tok);
    if (equalc(tok, "?")) {
        Token *qtok = tok;
        Node *cond = node;
        Node *then = expr(&tok, tok->next);
        tok = skip(tok, ":");
        Node *els = conditional(&tok, tok);
        node = new_node(ND_COND, qtok);
        node->cond = cond;
        node->then = then;
        node->els = els;
    }
    *rest = tok;
    return node;
}

static Node *assign(Token **rest, Token *tok) {
    Node *node = conditional(&tok, tok);
    if (equalc(tok, "="))
        node = new_binary(ND_ASSIGN, node, assign(&tok, tok->next), tok);
    else if (equalc(tok, "+="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_ADD, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "-="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SUB, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "*="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_MUL, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "/="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_DIV, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "%="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_MOD, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "&="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITAND, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "|="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITOR, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "^="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITXOR, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, "<<="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SHL, node, assign(&tok, tok->next), tok), tok);
    else if (equalc(tok, ">>="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SHR, node, assign(&tok, tok->next), tok), tok);
    *rest = tok;
    return node;
}

static Node *expr(Token **rest, Token *tok) {
    Node *node = assign(&tok, tok);
    while (equalc(tok, ","))
        node = new_binary(ND_COMMA, node, assign(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static LVar *parse_params(Token **rest, Token *tok, bool *is_variadic) {
    LVar head = {};
    LVar *cur = &head;
    int param_index = 0;

    *is_variadic = false;
    if (equalc(tok, "void") && equalc(tok->next, ")")) {
        *rest = tok->next;
        return NULL;
    }

    while (!equalc(tok, ")")) {
        if (cur != &head)
            tok = skip(tok, ",");
        if (equalc(tok, "...")) {
            *is_variadic = true;
            tok = tok->next;
            break;
        }

        VarAttr attr = {};
        Type *base = declspec(&tok, tok, &attr);
        char *name = NULL;
        Type *ty = declarator(&tok, tok, copy_type(base), &name);
        tok = skip_attributes(tok);

        if (!name)
            name = format("__param%d", param_index++);

        if (equalc(tok, "(")) {
            tok = tok->next;
            // Handle extra grouping parens: int ((int)) - outer ( consumed, tok = (int))
            bool stripped_extra = false;
            if (equalc(tok, "(") && (is_typename(tok->next) || equalc(tok->next, ")") || equalc(tok->next, "..."))) {
                stripped_extra = true;
                tok = tok->next;
            }
            bool dummy_variadic = false;
            LVar *nested_params = parse_params(&tok, tok, &dummy_variadic);
            tok = skip(tok, ")");
            if (stripped_extra)
                tok = skip(tok, ")");
            ty = func_type(ty);
            Type param_head = {};
            Type *pcur = &param_head;
            for (LVar *p = nested_params; p; p = p->param_next) {
                Type *pt = arena_alloc(sizeof(Type));
                *pt = *p->ty;
                pt->param_next = NULL;
                pcur->param_next = pt;
                pcur = pt;
            }
            ty->param_types = param_head.param_next;
            ty = pointer_to(ty);
        }

        if (ty->kind == TY_ARRAY)
            ty = pointer_to(ty->base);

        LVar *var = new_var(name, ty, true);
        cur = cur->param_next = var;
    }

    *rest = tok;
    return head.param_next;
}

static void global_initializer(Token **rest, Token *tok, LVar *var) {
    if (var->ty->kind == TY_ARRAY && var->ty->base->kind == TY_CHAR && tok->kind == TK_STR) {
        var->init_data = tok->str;
        var->init_size = strlen(tok->str) + 1;
        *rest = tok->next;
        return;
    }

    if (var->ty->kind == TY_PTR) {
        char *label = NULL;
        int addend = 0;
        if (read_global_label_initializer(&tok, tok, &label, &addend)) {
            var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
            var->init_size = var->ty->size;
            append_reloc(var, 0, label, addend);
            *rest = tok;
            return;
        }
        // Pointer initialized with &(compound literal): &(struct T){...}
        if (equalc(tok, "&") && find_compound_literal_start(tok->next)) {
            tok = tok->next; // skip &
            Token *compound_start = find_compound_literal_start(tok);
            Token *t = tok;
            while (equalc(t, "(")) t = t->next;
            Type *compound_ty = type_name(&t, t);
            while (equalc(t, ")")) t = t->next;
            static int anon_count;
            char *name = format(".Lanon.%d", anon_count++);
            LVar *anon_var = new_var(name, compound_ty, false);
            global_initializer(rest, compound_start, anon_var);
            tok = *rest;
            if (equalc(tok, "}"))
                tok = tok->next;
            while (equalc(tok, ")"))
                tok = tok->next;
            var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
            var->init_size = var->ty->size;
            append_reloc(var, 0, name, 0);
            *rest = tok;
            return;
        }
    }

    if (var->ty->kind == TY_ARRAY && equalc(tok, "{")) {
        int len = array_len(var->ty);
        if (len == 0) {
            Token *tmp = tok;
            len = count_array_initializer(&tmp, tmp, var->ty->base);
            var->ty = array_of(var->ty->base, len);
        }
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;
        *rest = global_init_one(tok, var, var->ty, 0);
        return;
    }

    if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && equalc(tok, "{")) {
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;
        tok = global_init_one(tok, var, var->ty, 0);
        *rest = tok;
        return;
    }

    // Struct/union initialized with a compound literal: (Type){...} or ((Type){...})
    if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && find_compound_literal_start(tok)) {
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;
        tok = global_init_one(tok, var, var->ty, 0);
        *rest = tok;
        return;
    }

    // Try to parse as an expression and evaluate
    {
        Node *node = assign(&tok, tok);
        add_type(node);

        // Try float constant evaluation for float types
        if (is_flonum(var->ty) || (node->ty && is_flonum(node->ty))) {
            double fv = 0;
            if (eval_double_const_expr(node, &fv)) {
                int sz = var->ty->size ? var->ty->size : 8;
                var->init_data = arena_alloc(sz);
                var->init_size = sz;
                if (sz == 4) {
                    float f = (float)fv;
                    memcpy(var->init_data, &f, 4);
                } else {
                    memcpy(var->init_data, &fv, 8);
                }
                *rest = tok;
                return;
            }
        }

        // Try integer constant evaluation
        long long ival = 0;
        if (eval_const_expr(node, &ival)) {
            var->has_init = true;
            var->init_val = (int64_t)ival;
            *rest = tok;
            return;
        }

        // For float comparisons stored in int (e.g. static int e1 = -1.0 == 0.0)
        {
            double fv = 0;
            if (eval_double_const_expr(node, &fv)) {
                var->has_init = true;
                var->init_val = (int64_t)fv;
                *rest = tok;
                return;
            }
        }

        error_tok(tok, "unsupported global initializer");
    }
}

static char *parse_toplevel_asm(Token **rest, Token *tok) {
    while (equalc(tok, "volatile") || equalc(tok, "__volatile__") ||
           equalc(tok, "__volatile") || equalc(tok, "goto"))
        tok = tok->next;
    tok = skip(tok, "(");
    if (tok->kind != TK_STR)
        error_tok(tok, "expected string literal in asm");
    char buf[4096];
    int pos = 0;
    while (tok->kind == TK_STR) {
        int n = tok->len;
        if (pos + n < (int)sizeof(buf)) {
            memcpy(buf + pos, tok->str, n);
            pos += n;
        }
        tok = tok->next;
    }
    buf[pos] = '\0';
    // Skip operand sections (outputs, inputs, clobbers, goto labels)
    while (!equalc(tok, ")")) {
        tok = skip(tok, ":");
        while (!equalc(tok, ":") && !equalc(tok, ")")) {
            if (equalc(tok, "[")) {
                tok = tok->next;
                if (tok->kind == TK_IDENT) tok = tok->next;
                if (equalc(tok, "]")) tok = tok->next;
                continue;
            }
            if (tok->kind == TK_STR) tok = tok->next;
            if (equalc(tok, "(")) {
                int depth = 1;
                tok = tok->next;
                while (depth > 0 && tok->kind != TK_EOF) {
                    if (equalc(tok, ")")) depth--;
                    else if (equalc(tok, "("))
                        depth++;
                    tok = tok->next;
                }
            }
            if (equalc(tok, ",")) tok = tok->next;
        }
    }
    tok = skip(tok, ")");
    *rest = tok;
    return str_intern(buf, pos);
}

Program *parse(Token *tok) {
    char *saved_input = current_input;
    char *saved_filename = current_filename;
    Token *head = tokenize("rcc_builtins",
                           "typedef struct {"
                           "  unsigned int gp_offset;"
                           "  unsigned int fp_offset;"
                           "  void *overflow_arg_area;"
                           "  void *reg_save_area;"
                           "} __builtin_va_list[1];");
    current_input = saved_input;
    current_filename = saved_filename;
    Token *t = head;
    while (t->next && t->next->kind != TK_EOF)
        t = t->next;
    t->next = tok;
    tok = head;

    globals = NULL;
    str_lits = NULL;
    TLItem item_head = {};
    TLItem *item_cur = &item_head;

    while (tok->kind != TK_EOF) {

        if (equalc(tok, "#") && equalc(tok->next, "pragma") &&
            equalc(tok->next->next, "pack")) {
            tok = tok->next->next->next; // skip '# pragma pack'
            if (equalc(tok, "(")) {
                tok = tok->next;
                if (tok->kind == TK_NUM)
                    pack_align = tok->val;
                else
                    pack_align = 0;
                tok = tok->next;
                if (equalc(tok, ")"))
                    tok = tok->next;
            }
            continue;
        }
        if (equalc(tok, "__asm__") || equalc(tok, "__asm") || equalc(tok, "asm")) {
            tok = tok->next;
            char *str = parse_toplevel_asm(&tok, tok);
            TLItem *item = arena_alloc(sizeof(TLItem));
            item->kind = TL_ASM;
            item->asm_str = str;
            item_cur = item_cur->next = item;
            tok = skip(tok, ";");
            continue;
        }

        if (equalc(tok, ";")) {
            tok = tok->next;
            continue;
        }

        VarAttr attr = {};
        Type *base = declspec(&tok, tok, &attr);

        if (equalc(tok, ";")) {
            tok = tok->next;
            continue;
        }

        for (;;) {
            char *name = NULL;
            Type *ty = declarator(&tok, tok, copy_type(base), &name);
            tok = skip_attributes(tok);

            if (!name) {
                tok = skip(tok, ";");
                break;
            }

            bool is_func = ty->kind == TY_FUNC || equalc(tok, "(");

            if (is_func) {
                Type *fty;
                bool is_variadic = false;
                LVar *params = NULL;
                fn_uses_vla = false;

                if (ty->kind == TY_FUNC) {
                    fty = ty;
                    is_variadic = ty->is_variadic;
                    locals = NULL;
                    stack_offset = 80;
                    LVar head = {};
                    LVar *cur = &head;
                    int param_index = 0;
                    for (Type *pt = ty->param_types; pt; pt = pt->param_next) {
                        char *pname = pt->name ? pt->name : format("__param%d", param_index++);
                        cur = cur->param_next = new_var(pname, pt, true);
                    }
                    params = head.param_next;
                } else {
                    fty = func_type(ty);
                    locals = NULL;
                    stack_offset = 80;
                    label_scopes = NULL;
                    pending_gotos = NULL;
                    current_switch = NULL;
                    current_loop = NULL;
                    parser_current_fn = name;

                    tok = tok->next;
                    if (!equalc(tok, ")") && !equalc(tok, "...") && !is_typename(tok)) {
                        // K&R function definition: param list has identifiers, not types
                        // First pass: collect parameter names and declarations
                        typedef struct KRParam KRParam;
                        struct KRParam {
                            KRParam *next;
                            char *name;
                            Type *ty;
                        };
                        KRParam kr_head = {};
                        KRParam *kr_cur = &kr_head;
                        while (!equalc(tok, ")")) {
                            if (kr_cur != &kr_head)
                                tok = skip(tok, ",");
                            if (tok->kind != TK_IDENT)
                                error_tok(tok, "expected parameter name");
                            KRParam *krp = arena_alloc(sizeof(KRParam));
                            krp->name = tok->name;
                            krp->ty = NULL;
                            tok = tok->next;
                            kr_cur = kr_cur->next = krp;
                        }
                        tok = skip(tok, ")");
                        tok = skip_attributes(tok);
                        // Parse K&R parameter declarations between ) and {, match by name
                        while (!equalc(tok, "{")) {
                            VarAttr dattr = {};
                            Type *dty = declspec(&tok, tok, &dattr);
                            for (;;) {
                                char *dname = NULL;
                                Type *ddecl = declarator(&tok, tok, copy_type(dty), &dname);
                                if (dname) {
                                    for (KRParam *krp = kr_head.next; krp; krp = krp->next) {
                                        if (strcmp(krp->name, dname) == 0) {
                                            krp->ty = ddecl;
                                            break;
                                        }
                                    }
                                }
                                if (!equalc(tok, ","))
                                    break;
                                tok = tok->next;
                            }
                            tok = skip(tok, ";");
                        }
                        // Second pass: create LVars with correct types and offsets
                        LVar head = {};
                        LVar *cur = &head;
                        for (KRParam *krp = kr_head.next; krp; krp = krp->next) {
                            if (!krp->ty)
                                krp->ty = ty_int;
                            LVar *var = new_var(krp->name, krp->ty, true);
                            cur = cur->param_next = var;
                        }
                        params = head.param_next;
                        current_fn_scope_locals = params;
                    } else {
                        params = parse_params(&tok, tok, &is_variadic);
                        tok = skip(tok, ")");
                        tok = skip_attributes(tok);
                        current_fn_scope_locals = params;
                    }

                    // Build parameter type list
                    fty->is_variadic = is_variadic;
                    Type param_head = {};
                    Type *pcur = &param_head;
                    for (LVar *p = params; p; p = p->param_next) {
                        Type *pt = arena_alloc(sizeof(Type));
                        *pt = *p->ty;
                        pt->param_next = NULL;
                        pcur = pcur->param_next = pt;
                    }
                    fty->param_types = param_head.param_next;
                }

                label_scopes = NULL;
                pending_gotos = NULL;
                current_switch = NULL;
                current_loop = NULL;
                parser_current_fn = name;
                current_fn_scope_locals = params;
                current_block_depth = 0;
                suppress_fn_scope_update = false;

                if (fty->return_ty && (fty->return_ty->kind == TY_STRUCT || fty->return_ty->kind == TY_UNION)) {
                    LVar *retbuf = new_var("", pointer_to(fty->return_ty), true);
                    retbuf->cleanup_func = NULL;
                }

                // For typedefs like 'typedef int functype(int);', register the type
                if (attr.is_typedef) {
                    add_typedef(name, fty);
                    if (!equalc(tok, ";") && !equalc(tok, ","))
                        error_tok(tok, "expected ';' or ',' after typedef");
                } else {
                    // Register function symbol
                    Type *fn_symbol_ty = pointer_to(fty);
                    LVar *existing = find_global_name(name);
                    LVar *fn_lvar = existing;
                    if (!existing) {
                        fn_lvar = new_var(name, fn_symbol_ty, false);
                        fn_lvar->is_extern = attr.is_extern || (!attr.is_inline && !attr.is_static);
                        fn_lvar->is_function = true;
                        fn_lvar->is_inline = attr.is_inline;
                        fn_lvar->is_weak = attr.is_weak;
                        fn_lvar->is_static = attr.is_static;
                        if (pending_asm_name)
                            fn_lvar->asm_name = pending_asm_name;
                        if (pending_alias_target) {
                            fn_lvar->alias_target = pending_alias_target;
                            pending_alias_target = NULL;
                        }
                        // A declaration without inline (and without static) makes the
                        // function an external symbol even if a prior inline def exists.
                        if (!attr.is_inline && !attr.is_static)
                            fn_lvar->has_init = true; // reuse has_init as "has non-inline decl"
                    } else {
                        existing->ty = fn_symbol_ty;
                        // Update flags on redeclaration
                        if (attr.is_inline)
                            existing->is_inline = true;
                        if (attr.is_weak)
                            existing->is_weak = true;
                        if (attr.is_static)
                            existing->is_static = true;
                        if (attr.is_extern)
                            existing->is_extern = true;
                        if (pending_asm_name)
                            existing->asm_name = pending_asm_name;
                        if (pending_alias_target) {
                            existing->alias_target = pending_alias_target;
                            pending_alias_target = NULL;
                        }
                        if (!attr.is_inline && !attr.is_static)
                            existing->has_init = true; // non-inline extern decl seen
                    }
                }

                if (equalc(tok, "{")) {
                    if (attr.is_typedef)
                        error_tok(tok, "typedef cannot have function body");

                    LVar *fn_locals = NULL;
                    Node *body = compound_stmt_ex(&tok, tok, &fn_locals);
                    // Implicit return 0 for main if no explicit return
                    if (strcmp(name, "main") == 0) {
                        Node *last = body->body;
                        if (last) {
                            while (last->next)
                                last = last->next;
                            if (last->kind != ND_RETURN) {
                                Node *ret = new_node(ND_RETURN, tok);
                                ret->lhs = new_num(0, tok);
                                last->next = ret;
                            }
                        }
                    }
                    Function *fn = arena_alloc(sizeof(Function));
                    fn->name = name;
                    LVar *fn_sym2 = find_global_name(name);
                    fn->asm_name = pending_asm_name ? pending_asm_name
                                                    : (fn_sym2 ? fn_sym2->asm_name : NULL);
                    fn->alias_target = pending_alias_target;
                    fn->ty = fty;
                    fn->params = params;
                    fn->locals = fn_locals;
                    fn->body = body->body;
                    fn->stack_size = align_to(stack_offset, 16);
                    fn->is_variadic = is_variadic;
                    fn->dealloc_vla = fn_uses_vla;
                    fn->is_constructor = pending_constructor;
                    fn->is_destructor = pending_destructor;
                    fn->is_inline = attr.is_inline;
                    // is_static is sticky: if any decl was static the fn is static
                    fn->is_static = attr.is_static || (fn_sym2 && fn_sym2->is_static);
                    // is_extern: explicit extern on this def, OR any non-inline extern
                    // declaration seen (has_init flag).
                    fn->is_extern = attr.is_extern || (fn_sym2 && fn_sym2->has_init);
                    fn->is_weak = attr.is_weak || (fn_sym2 && fn_sym2->is_weak);
                    pending_constructor = false;
                    pending_destructor = false;
                    pending_asm_name = NULL;
                    pending_alias_target = NULL;
                    TLItem *item = arena_alloc(sizeof(TLItem));
                    item->kind = TL_FUNC;
                    item->fn = fn;
                    item_cur = item_cur->next = item;
                    current_fn_scope_locals = NULL;
                    current_block_depth = 0;
                    suppress_fn_scope_update = false;
                    break;
                }

                if (equalc(tok, ";")) {
                    tok = tok->next;
                    break;
                }
                if (equalc(tok, ",")) {
                    tok = tok->next;
                    continue;
                }
                error_tok(tok, "expected ';', ',', or '{'");
            } else {
                if (attr.is_typedef) {
                    add_typedef(name, ty);
                } else {
                    if (equalc(tok, "="))
                        ty = infer_array_type(ty, tok->next);
                    LVar *var = find_global_name(name);
                    if (var) {
                        if (var->ty->kind == TY_ARRAY && ty->kind == TY_ARRAY && var->ty->size > 0)
                            ty = var->ty;
                        else
                            var->ty = ty;
                    } else {
                        var = new_var(name, ty, false);
                    }
                    var->is_extern = attr.is_extern;
                    if (pending_asm_name)
                        var->asm_name = pending_asm_name;
                    if (pending_alias_target)
                        var->alias_target = pending_alias_target;
                    pending_asm_name = NULL;
                    pending_alias_target = NULL;
                    if (equalc(tok, "=")) {
                        tok = tok->next;
                        global_initializer(&tok, tok, var);
                    }
                }

                if (equalc(tok, ";")) {
                    tok = tok->next;
                    break;
                }
                if (equalc(tok, ",")) {
                    tok = tok->next;
                    continue;
                }
                error_tok(tok, "expected ';' or ','");
            }
        }
    }

    Program *prog = arena_alloc(sizeof(Program));
    prog->items = item_head.next;
    prog->globals = globals;
    prog->strs = str_lits;
    return prog;
}
