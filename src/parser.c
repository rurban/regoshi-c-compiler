#include "rcc.h"

typedef struct VarAttr VarAttr;
typedef struct TagScope TagScope;
typedef struct EnumConst EnumConst;

struct VarAttr {
    bool is_typedef;
    bool is_extern;
    bool is_static;
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

static StrLit *str_lits;
static int str_lit_counter;

static Node *current_switch;
static int static_local_counter;
static Node *conditional(Token **rest, Token *tok);

static bool equal(Token *tok, char *op) {
    return memcmp(tok->loc, op, tok->len) == 0 && op[tok->len] == '\0';
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
    if (val >= -2147483648LL && val <= 2147483647LL)
        node->ty = ty_int;
    else if (val >= 0 && val <= 0xFFFFFFFFLL)
        node->ty = ty_llong;  // On LLP64, values > INT_MAX go to llong
    else
        node->ty = ty_llong;
    return node;
}

static Node *new_fnum(double fval, Token *tok) {
    Node *node = new_node(ND_FNUM, tok);
    node->fval = fval;
    node->ty = ty_double;
    return node;
}

static Type *copy_type(Type *ty) {
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

static Typedef *find_typedef(Token *tok) {
    for (Typedef *td = typedefs; td; td = td->next)
        if (equal(tok, td->name))
            return td;
    return NULL;
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

static Member *find_member(Type *ty, Token *tok) {
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        error_tok(tok, "not a struct or union");
    for (Member *mem = ty->members; mem; mem = mem->next)
        if (equal(tok, mem->name))
            return mem;
    return NULL;
}

static StrLit *new_str_lit(char *str) {
    StrLit *s = arena_alloc(sizeof(StrLit));
    s->str = str;
    s->id = str_lit_counter++;
    s->next = str_lits;
    str_lits = s;
    return s;
}

static bool is_storage_class(Token *tok) {
    return equal(tok, "typedef") || equal(tok, "extern") || equal(tok, "static") ||
           equal(tok, "inline") || equal(tok, "__inline") || equal(tok, "__inline__") ||
           equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") || equal(tok, "signed") ||
           equal(tok, "unsigned") || equal(tok, "short") || equal(tok, "long");
}

static Token *skip_balanced(Token *tok) {
    int depth = 0;
    do {
        if (equal(tok, "("))
            depth++;
        else if (equal(tok, ")"))
            depth--;
        tok = tok->next;
    } while (depth > 0 && tok->kind != TK_EOF);
    return tok;
}

static Type *type_name(Token **rest, Token *tok);

static Token *read_type_attrs(Token *tok, int *align);

static Token *skip_attributes(Token *tok) {
    return read_type_attrs(tok, NULL);
}

static Token *skip_type_quals(Token *tok) {
    while (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") ||
           equal(tok, "__restrict") || equal(tok, "__restrict__"))
        tok = tok->next;
    return tok;
}

static bool is_typename(Token *tok) {
    if (equal(tok, "__attribute__") || equal(tok, "__attribute") ||
        equal(tok, "__declspec") || equal(tok, "_Alignas"))
        return true;
    tok = skip_attributes(tok);
    if (equal(tok, "int") || equal(tok, "char") || equal(tok, "void") ||
        equal(tok, "float") || equal(tok, "double") ||
        equal(tok, "_Bool") || equal(tok, "struct") || equal(tok, "union") || equal(tok, "enum") ||
        equal(tok, "typeof") || equal(tok, "__typeof") || equal(tok, "__typeof__"))
        return true;
    if (is_storage_class(tok))
        return true;
    return find_typedef(tok) != NULL;
}

static Type *declspec(Token **rest, Token *tok, VarAttr *attr);
static Node *expr(Token **rest, Token *tok);
static bool eval_const_expr(Node *node, long long *val);
static void global_initializer(Token **rest, Token *tok, LVar *var);

static void maybe_update_align(int *align, int value) {
    if (align && value > *align)
        *align = value;
}

static Token *read_type_attrs(Token *tok, int *align) {
    while (true) {
        if (equal(tok, "_Alignas")) {
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

        if (equal(tok, "__attribute__") || equal(tok, "__attribute")) {
            tok = tok->next;
            tok = skip(tok, "(");
            tok = skip(tok, "(");
            while (!(equal(tok, ")") && equal(tok->next, ")"))) {
                if (equal(tok, "aligned") || equal(tok, "__aligned__")) {
                    tok = tok->next;
                    if (equal(tok, "(")) {
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
                    if (equal(tok, ","))
                        tok = tok->next;
                    continue;
                }

                if (equal(tok, "(")) {
                    tok = skip_balanced(tok);
                } else {
                    tok = tok->next;
                }

                if (equal(tok, ","))
                    tok = tok->next;
            }
            tok = skip(tok, ")");
            tok = skip(tok, ")");
            continue;
        }

        if (equal(tok, "__declspec")) {
            tok = tok->next;
            if (equal(tok, "("))
                tok = skip_balanced(tok);
            continue;
        }

        break;
    }

    return tok;
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

static Type *type_suffix(Token **rest, Token *tok, Type *ty) {
    while (equal(tok, "[")) {
        tok = tok->next;
        int len = 0;
        while (equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict") || equal(tok, "static"))
            tok = tok->next;
        if (!equal(tok, "]")) {
            if (equal(tok, "*")) {
                tok = tok->next;
            } else {
                Node *node = expr(&tok, tok);
                long long val = 0;
                if (!eval_const_expr(node, &val))
                    error_tok(tok, "expected array size");
                len = (int)val;
            }
        }
        tok = skip(tok, "]");
        ty = array_of(ty, len);
    }
    *rest = tok;
    return ty;
}

static Type *declarator(Token **rest, Token *tok, Type *ty, char **name) {
    int decl_align = 0;
    tok = read_type_attrs(tok, &decl_align);
    while (equal(tok, "*")) {
        ty = pointer_to(ty);
        tok = tok->next;
        tok = read_type_attrs(tok, &decl_align);
        tok = skip_type_quals(tok);
    }

    if (equal(tok, "(") && equal(tok->next, "*")) {
        int ptr_count = 0;
        tok = tok->next;
        while (equal(tok, "*")) {
            ptr_count++;
            tok = tok->next;
            tok = read_type_attrs(tok, &decl_align);
            tok = skip_type_quals(tok);
        }
        tok = skip_type_quals(tok);

        if (tok->kind == TK_IDENT) {
            if (name)
                *name = tok->name;
            tok = tok->next;
        } else if (name) {
            *name = NULL;
        }

        tok = skip(tok, ")");
        if (equal(tok, "(")) {
            int depth = 0;
            do {
                if (equal(tok, "("))
                    depth++;
                else if (equal(tok, ")"))
                    depth--;
                tok = tok->next;
            } while (depth > 0 && tok->kind != TK_EOF);
            ty = func_type(ty);
        }

        while (ptr_count-- > 0)
            ty = pointer_to(ty);
        ty = type_suffix(rest, tok, ty);
        return apply_type_align(ty, decl_align);
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
    tok = read_type_attrs(tok, &decl_align);
    ty = type_suffix(rest, tok, ty);
    return apply_type_align(ty, decl_align);
}

static Type *enum_specifier(Token **rest, Token *tok) {
    tok = skip(tok, "enum");
    if (tok->kind == TK_IDENT)
        tok = tok->next;

    if (!equal(tok, "{")) {
        *rest = tok;
        return ty_int;
    }

    tok = tok->next;
    int val = 0;
    while (!equal(tok, "}")) {
        if (tok->kind != TK_IDENT)
            error_tok(tok, "expected enum constant");

        EnumConst *ec = arena_alloc(sizeof(EnumConst));
        ec->name = tok->name;
        tok = tok->next;

        if (equal(tok, "=")) {
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

        if (!equal(tok, "}"))
            tok = skip(tok, ",");
    }

    *rest = tok->next;
    return ty_int;
}

static Type *struct_or_union_specifier(Token **rest, Token *tok, bool is_union) {
    tok = tok->next;
    tok = skip_attributes(tok);

    Token *tag_tok = NULL;
    if (tok->kind == TK_IDENT) {
        tag_tok = tok;
        tok = tok->next;
    }

    Type *ty = NULL;
    if (tag_tok) {
        TagScope *tag = find_tag(tag_tok);
        if (tag) {
            ty = tag->ty;
        } else {
            ty = arena_alloc(sizeof(Type));
            ty->kind = is_union ? TY_UNION : TY_STRUCT;
            ty->size = 0;
            ty->align = 1;
            push_tag(tag_tok->name, ty);
        }
    } else {
        ty = arena_alloc(sizeof(Type));
        ty->kind = is_union ? TY_UNION : TY_STRUCT;
        ty->size = 0;
        ty->align = 1;
    }

    if (!equal(tok, "{")) {
        *rest = tok;
        return ty;
    }

    tok = tok->next;
    Member head = {};
    Member *cur = &head;
    int offset = 0;
    int max_size = 0;
    int max_align = 1;

    while (!equal(tok, "}")) {
        VarAttr attr = {};
        Type *base = declspec(&tok, tok, &attr);
        if (attr.is_typedef || attr.is_extern || attr.is_static)
            error_tok(tok, "invalid storage class in member declaration");
        if (!base)
            error_tok(tok, "expected member type");
        if (equal(tok, ";")) {
            tok = tok->next;
            continue;
        }

        for (;;) {
            char *name = NULL;
            Type *mem_ty = declarator(&tok, tok, copy_type(base), &name);
            tok = skip_attributes(tok);
            if (!name)
                error_tok(tok, "expected member name");

            Member *mem = arena_alloc(sizeof(Member));
            mem->name = name;
            mem->ty = mem_ty;
            if (is_union) {
                mem->offset = 0;
                if (max_size < mem_ty->size)
                    max_size = mem_ty->size;
            } else {
                offset = align_to(offset, mem_ty->align);
                mem->offset = offset;
                offset += mem_ty->size;
            }
            if (max_align < mem_ty->align)
                max_align = mem_ty->align;
            cur = cur->next = mem;

            if (!equal(tok, ","))
                break;
            tok = tok->next;
        }

        tok = skip(tok, ";");
    }

    tok = skip(tok, "}");
    ty->members = head.next;
    ty->align = max_align;
    ty->size = is_union ? align_to(max_size, max_align) : align_to(offset, max_align);
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
    memset(attr, 0, sizeof(*attr));

    for (;;) {
        Token *attr_tok = read_type_attrs(tok, &attr_align);
        if (attr_tok != tok) {
            tok = attr_tok;
            continue;
        }

        if (equal(tok, "typedef")) {
            attr->is_typedef = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "extern")) {
            attr->is_extern = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "static")) {
            attr->is_static = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "inline") || equal(tok, "__inline") || equal(tok, "__inline__") ||
            equal(tok, "const") || equal(tok, "volatile") || equal(tok, "restrict")) {
            tok = tok->next;
            continue;
        }
        if (equal(tok, "signed")) {
            is_signed = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "unsigned")) {
            is_unsigned = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "short")) {
            is_short = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "long")) {
            long_count++;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "int")) {
            is_int = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "char")) {
            is_char = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "float")) {
            is_float = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "double")) {
            is_double = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "_Bool")) {
            is_bool = true;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "__int64")) {
            long_count = 2;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "void")) {
            is_void = true;
            tok = tok->next;
            continue;
        }

        if (equal(tok, "typeof") || equal(tok, "__typeof") || equal(tok, "__typeof__")) {
            tok = tok->next;
            tok = skip(tok, "(");
            if (is_typename(tok)) {
                ty = type_name(&tok, tok);
            } else {
                Node *node = expr(&tok, tok);
                add_type(node);
                ty = node->ty;
            }
            tok = skip(tok, ")");
            continue;
        }

        Typedef *td = find_typedef(tok);
        if (td) {
            ty = td->ty;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "struct")) {
            ty = struct_or_union_specifier(&tok, tok, false);
            continue;
        }
        if (equal(tok, "union")) {
            ty = struct_or_union_specifier(&tok, tok, true);
            continue;
        }
        if (equal(tok, "enum")) {
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
        } else if (is_short) {
            ty = is_unsigned ? ty_ushort : ty_short;
        } else if (long_count >= 2) {
            ty = is_unsigned ? ty_ullong : ty_llong;
        } else if (long_count == 1) {
            ty = is_unsigned ? ty_ulong : ty_long;
        } else if (is_int || is_signed || is_unsigned) {
            ty = is_unsigned ? ty_uint : ty_int;
        } else if (attr_align > 0) {
            ty = ty_int;
        }
    }

    if (!ty)
        error_tok(tok, "expected type name");

    ty = apply_type_align(ty, attr_align);
    tok = skip_attributes(tok);
    tok = skip_type_quals(tok);
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
    if (!equal(tok, "("))
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

    if (!var->relocs) {
        var->relocs = rel;
        return;
    }

    Reloc *cur = var->relocs;
    while (cur->next)
        cur = cur->next;
    cur->next = rel;
}

static bool read_global_label_initializer(Token **rest, Token *tok, char **label) {
    if (tok->kind == TK_STR) {
        StrLit *s = new_str_lit(tok->str);
        *label = format(".LC%d", s->id);
        *rest = tok->next;
        return true;
    }

    if (equal(tok, "&"))
        tok = tok->next;

    if (tok->kind == TK_IDENT) {
        *label = tok->name;
        *rest = tok->next;
        return true;
    }

    return false;
}

static Token *skip_initializer(Token *tok) {
    if (!equal(tok, "{")) {
        assign(&tok, tok);
        return tok;
    }

    int depth = 0;
    do {
        if (equal(tok, "{"))
            depth++;
        else if (equal(tok, "}"))
            depth--;
        tok = tok->next;
    } while (depth > 0 && tok->kind != TK_EOF);
    return tok;
}

static int count_array_initializer(Token **rest, Token *tok) {
    int count = 0;
    tok = skip(tok, "{");
    while (!equal(tok, "}")) {
        tok = skip_initializer(tok);
        count++;
        if (equal(tok, ",")) {
            tok = tok->next;
            if (equal(tok, "}"))
                break;
            continue;
        }
        break;
    }
    *rest = skip(tok, "}");
    return count;
}

static bool is_wide_string_token(Token *tok) {
    return tok && tok->kind == TK_IDENT &&
           (equal(tok, "L") || equal(tok, "u") || equal(tok, "U")) &&
           tok->next && tok->next->kind == TK_STR;
}

static Type *infer_array_type(Type *ty, Token *tok) {
    if (!ty || ty->kind != TY_ARRAY || ty->size != 0)
        return ty;
    if (tok->kind == TK_STR)
        return array_of(ty->base, (int)strlen(tok->str) + 1);
    if (is_wide_string_token(tok))
        return array_of(ty->base, (int)strlen(tok->next->str) + 1);
    if (equal(tok, "{")) {
        Token *tmp = tok;
        return array_of(ty->base, count_array_initializer(&tmp, tmp));
    }
    return ty;
}

static Node *new_array_elem_lvalue(LVar *var, int idx, Token *tok) {
    Node *base = new_var_node(var, tok);
    Node *offset = new_num(idx, tok);
    return new_unary(ND_DEREF, new_binary(ND_ADD, base, offset, tok), tok);
}

static Node *local_array_initializer(Token **rest, Token *tok, LVar *var) {
    Node head = {};
    Node *cur = &head;
    int idx = 0;
    tok = skip(tok, "{");
    while (!equal(tok, "}")) {
        if (equal(tok, "{")) {
            tok = skip_initializer(tok);
            idx++;
        } else {
            Token *start = tok;
            Node *lhs = new_array_elem_lvalue(var, idx++, tok);
            Node *rhs = assign(&tok, tok);
            cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
        }
        if (equal(tok, ",")) {
            tok = tok->next;
            if (equal(tok, "}"))
                break;
            continue;
        }
        break;
    }
    *rest = skip(tok, "}");
    return head.next;
}

static int64_t read_const_initializer(Token **rest, Token *tok) {
    Node *node = assign(&tok, tok);
    add_type(node);
    long long val = 0;
    if (eval_const_expr(node, &val)) {
        *rest = tok;
        return (int64_t)val;
    }
    // Fallback for simple cases
    error_tok(tok, "expected constant expression in initializer");
    return 0;
}

static void write_scalar_bytes(char *buf, int offset, int size, int64_t val) {
    if (size == 1) {
        buf[offset] = (char)val;
        return;
    }
    if (size == 2) {
        int16_t v = (int16_t)val;
        memcpy(buf + offset, &v, 2);
        return;
    }
    if (size == 4) {
        int32_t v = (int32_t)val;
        memcpy(buf + offset, &v, 4);
        return;
    }
    int64_t v = val;
    memcpy(buf + offset, &v, 8);
}

static Node *declaration(Token **rest, Token *tok) {
    VarAttr attr = {};
    Type *base = declspec(&tok, tok, &attr);
    Node head = {};
    Node *cur = &head;

    if (equal(tok, ";")) {
        *rest = tok->next;
        return new_node(ND_NULL, tok);
    }

    while (!equal(tok, ";")) {
        char *name = NULL;
        Type *ty = declarator(&tok, tok, copy_type(base), &name);
        tok = skip_attributes(tok);
        if (!name)
            error_tok(tok, "expected variable name");

        if (equal(tok, "(")) {
            tok = skip_balanced(tok);
            if (!find_global_name(name)) {
                LVar *fn_sym = new_var(name, pointer_to(func_type(ty)), false);
                fn_sym->is_extern = true;
                fn_sym->is_function = true;
            }
            if (!equal(tok, ","))
                break;
            tok = tok->next;
            continue;
        }

        if (attr.is_typedef) {
            Typedef *td = arena_alloc(sizeof(Typedef));
            td->name = name;
            td->ty = ty;
            td->next = typedefs;
            typedefs = td;
        } else if (attr.is_static) {
            // Static local variable: create global storage with unique name
            char *asm_label = format(".Lstatic.%d", static_local_counter++);
            if (equal(tok, "="))
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
            lvar->asm_name = asm_label;
            lvar->ty = ty;
            lvar->is_local = false;
            lvar->is_static = true;
            lvar->next = locals;
            locals = lvar;
            if (equal(tok, "=")) {
                tok = tok->next;
                global_initializer(&tok, tok, gvar);
            }
        } else {
            if (equal(tok, "="))
                ty = infer_array_type(ty, tok->next);
            LVar *var = new_var(name, ty, true);
            if (equal(tok, "=")) {
                Token *start = tok;
                tok = tok->next;
                if (var->ty->kind == TY_ARRAY) {
                    if (equal(tok, "{")) {
                        cur->next = local_array_initializer(&tok, tok, var);
                        while (cur->next)
                            cur = cur->next;
                    } else if (is_wide_string_token(tok)) {
                        if (var->ty->base->kind == TY_CHAR) {
                            Node *lhs = new_var_node(var, start);
                            Node *rhs = assign(&tok, tok->next);
                            cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                        } else {
                            tok = tok->next->next;
                        }
                    } else if (tok->kind == TK_STR && var->ty->base->kind != TY_CHAR) {
                        tok = tok->next;
                    } else {
                        Node *lhs = new_var_node(var, start);
                        Node *rhs = assign(&tok, tok);
                        cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                    }
                } else {
                    Node *lhs = new_var_node(var, start);
                    Node *rhs = assign(&tok, tok);
                    cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                }
            }
        }

        if (!equal(tok, ","))
            break;
        tok = tok->next;
    }

    *rest = skip(tok, ";");
    return head.next ? head.next : new_node(ND_NULL, tok);
}

static Node *compound_stmt(Token **rest, Token *tok) {
    LVar *saved_locals = locals;
    Typedef *saved_typedefs = typedefs;
    TagScope *saved_tags = tags;
    EnumConst *saved_enum_consts = enum_consts;

    Node head = {};
    Node *cur = &head;
    tok = skip(tok, "{");

    while (!equal(tok, "}")) {
        if (is_typename(tok)) {
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

    locals = saved_locals;
    typedefs = saved_typedefs;
    tags = saved_tags;
    enum_consts = saved_enum_consts;
    return node;
}

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN, tok);
        if (equal(tok->next, ";")) {
            *rest = tok->next->next;
            return node;
        }
        node->lhs = expr(&tok, tok->next);
        *rest = skip(tok, ";");
        return node;
    }

    if (equal(tok, "if")) {
        Node *node = new_node(ND_IF, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        if (equal(tok, "else"))
            node->els = stmt(&tok, tok->next);
        *rest = tok;
        return node;
    }

    if (equal(tok, "while")) {
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        *rest = tok;
        return node;
    }

    if (equal(tok, "do")) {
        Node *node = new_node(ND_DO, tok);
        node->then = stmt(&tok, tok->next);
        tok = skip(tok, "while");
        tok = skip(tok, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        *rest = skip(tok, ";");
        return node;
    }

    if (equal(tok, "for")) {
        LVar *saved_locals = locals;
        Typedef *saved_typedefs = typedefs;
        Node *node = new_node(ND_FOR, tok);
        tok = skip(tok->next, "(");

        if (!equal(tok, ";")) {
            if (is_typename(tok)) {
                node->init = declaration(&tok, tok);
            } else {
                node->init = expr(&tok, tok);
                tok = skip(tok, ";");
            }
        } else {
            tok = tok->next;
        }

        if (!equal(tok, ";"))
            node->cond = expr(&tok, tok);
        tok = skip(tok, ";");

        if (!equal(tok, ")"))
            node->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        node->then = stmt(&tok, tok);
        locals = saved_locals;
        typedefs = saved_typedefs;
        *rest = tok;
        return node;
    }

    if (equal(tok, "switch")) {
        Node *node = new_node(ND_SWITCH, tok);
        tok = skip(tok->next, "(");
        node->cond = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *saved = current_switch;
        current_switch = node;
        node->then = stmt(&tok, tok);
        current_switch = saved;
        *rest = tok;
        return node;
    }

    if (equal(tok, "case")) {
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
        if (equal(tok, "...")) {
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

    if (equal(tok, "default")) {
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

    if (equal(tok, "break")) {
        *rest = skip(tok->next, ";");
        return new_node(ND_BREAK, tok);
    }

    if (equal(tok, "continue")) {
        *rest = skip(tok->next, ";");
        return new_node(ND_CONTINUE, tok);
    }

    if (equal(tok, "goto")) {
        Node *node = new_node(ND_GOTO, tok);
        tok = tok->next;
        if (tok->kind != TK_IDENT)
            error_tok(tok, "expected label name");
        node->label_name = tok->name;
        *rest = skip(tok->next, ";");
        return node;
    }

    if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
        Node *node = new_node(ND_LABEL, tok);
        node->label_name = tok->name;
        tok = tok->next->next;
        tok = skip_attributes(tok);
        node->lhs = stmt(&tok, tok);
        *rest = tok;
        return node;
    }

    if (equal(tok, "{"))
        return compound_stmt(rest, tok);

    if (equal(tok, ";")) {
        *rest = tok->next;
        return new_node(ND_NULL, tok);
    }

    Node *node = new_node(ND_EXPR_STMT, tok);
    node->lhs = expr(&tok, tok);
    *rest = skip(tok, ";");
    return node;
}

static Node *primary(Token **rest, Token *tok) {
    Node *node = NULL;

    if (equal(tok, "(")) {
        if (equal(tok->next, "{")) {
            node = new_node(ND_STMT_EXPR, tok);
            Node *block = compound_stmt(&tok, tok->next);
            node->body = block->body;
            tok = skip(tok, ")");
        } else {
            node = expr(&tok, tok->next);
            tok = skip(tok, ")");
        }
    } else if (tok->kind == TK_IDENT) {
        if (equal(tok->next, "(")) {
            node = new_node(ND_FUNCALL, tok);
            LVar *var = find_var(tok);
            if (var)
                node->lhs = new_var_node(var, tok);
            else
                node->funcname = tok->name;
            tok = skip(tok->next, "(");
            Node head = {};
            Node *cur = &head;
            while (!equal(tok, ")")) {
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
            } else if (equal(tok, "NULL")) {
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
        StrLit *s = new_str_lit(tok->str);
        node->str_id = s->id;
        tok = tok->next;
    } else {
        error_tok(tok, "expected an expression");
    }

    add_type(node);

    while (true) {
        if (equal(tok, "(")) {
            Node *call = new_node(ND_FUNCALL, tok);
            call->lhs = node;
            tok = tok->next;
            Node head = {};
            Node *cur = &head;
            while (!equal(tok, ")")) {
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
        if (equal(tok, "[")) {
            Token *start = tok;
            Node *idx = expr(&tok, tok->next);
            tok = skip(tok, "]");
            node = new_unary(ND_DEREF, new_binary(ND_ADD, node, idx, start), start);
            add_type(node);
            continue;
        }
        if (equal(tok, ".")) {
            tok = tok->next;
            add_type(node);
            Member *mem = find_member(node->ty, tok);
            if (!mem)
                error_tok(tok, "no such member");
            Node *mem_node = new_unary(ND_MEMBER, node, tok);
            mem_node->member = mem;
            mem_node->ty = mem->ty;
            node = mem_node;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "->")) {
            tok = tok->next;
            add_type(node);
            if (node->ty->kind != TY_PTR ||
                (node->ty->base->kind != TY_STRUCT && node->ty->base->kind != TY_UNION))
                error_tok(tok, "not a pointer to struct or union");
            node = new_unary(ND_DEREF, node, tok);
            add_type(node);
            Member *mem = find_member(node->ty, tok);
            if (!mem)
                error_tok(tok, "no such member");
            Node *mem_node = new_unary(ND_MEMBER, node, tok);
            mem_node->member = mem;
            mem_node->ty = mem->ty;
            node = mem_node;
            tok = tok->next;
            continue;
        }
        if (equal(tok, "++")) {
            node = new_unary(ND_POST_INC, node, tok);
            tok = tok->next;
            add_type(node);
            continue;
        }
        if (equal(tok, "--")) {
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

static Node *unary(Token **rest, Token *tok) {
    if (equal(tok, "__builtin_offsetof")) {
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

            if (equal(tok, "[")) {
                tok = tok->next;
                if (tok->kind != TK_NUM || ty->kind != TY_ARRAY)
                    error_tok(tok, "unsupported offsetof designator");
                offset += tok->val * ty->base->size;
                ty = ty->base;
                tok = skip(tok->next, "]");
            }

            if (!equal(tok, "."))
                break;
            tok = tok->next;
        }

        *rest = skip(tok, ")");
        return new_num(offset, start);
    }
    if (equal(tok, "++")) {
        Token *start = tok;
        Node *lhs = unary(&tok, tok->next);
        *rest = tok;
        return new_binary(ND_ASSIGN, lhs, new_binary(ND_ADD, lhs, new_num(1, start), start), start);
    }
    if (equal(tok, "--")) {
        Token *start = tok;
        Node *lhs = unary(&tok, tok->next);
        *rest = tok;
        return new_binary(ND_ASSIGN, lhs, new_binary(ND_SUB, lhs, new_num(1, start), start), start);
    }
    if (equal(tok, "+"))
        return unary(rest, tok->next);
    if (equal(tok, "-"))
        return new_unary(ND_NEG, unary(rest, tok->next), tok);
    if (equal(tok, "!"))
        return new_unary(ND_NOT, unary(rest, tok->next), tok);
    if (equal(tok, "~"))
        return new_unary(ND_BITNOT, unary(rest, tok->next), tok);
    if (equal(tok, "&"))
        return new_unary(ND_ADDR, unary(rest, tok->next), tok);
    if (equal(tok, "*"))
        return new_unary(ND_DEREF, unary(rest, tok->next), tok);
    if (equal(tok, "sizeof")) {
        if (equal(tok->next, "(") && is_typename(tok->next->next)) {
            Type *ty = parse_cast_type(&tok, tok->next);
            *rest = tok;
            return new_num(ty->size, tok);
        }
        Node *node = unary(&tok, tok->next);
        add_type(node);
        *rest = tok;
        return new_num(node->ty->size, tok);
    }
    if (equal(tok, "__alignof__") || equal(tok, "__alignof")) {
        Token *start = tok;
        if (equal(tok->next, "(") && is_typename(tok->next->next)) {
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
        if (equal(tok, "{")) {
            tok = tok->next;

            // For incomplete arrays, count elements first
            if (ty->kind == TY_ARRAY && ty->size == 0 && ty->base) {
                Token *tmp = tok;
                int count = 0;
                int depth = 0;
                while (true) {
                    if (equal(tmp, "{")) depth++;
                    else if (equal(tmp, "}")) {
                        if (depth == 0) break;
                        depth--;
                    }
                    if (depth == 0 && (equal(tmp, ",") || equal(tmp, "}")))
                        ;
                    else
                        count++;
                    // Advance past comma-separated items
                    if (depth == 0 && equal(tmp->next, ",")) {
                        tmp = tmp->next->next;
                        continue;
                    }
                    if (depth == 0 && equal(tmp->next, "}")) {
                        tmp = tmp->next;
                        continue;
                    }
                    tmp = tmp->next;
                }
                // Simple count: count commas + 1
                tmp = tok;
                count = 1;
                depth = 0;
                while (!(depth == 0 && equal(tmp, "}"))) {
                    if (equal(tmp, "{")) depth++;
                    else if (equal(tmp, "}")) depth--;
                    else if (depth == 0 && equal(tmp, ",")) count++;
                    tmp = tmp->next;
                }
                // Handle trailing comma
                Token *before_end = tok;
                for (Token *t = tok; !equal(t, "}"); t = t->next)
                    before_end = t;
                if (equal(before_end, ","))
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
                while (!equal(tok, "}")) {
                    Node *idx = new_num(i, start);
                    Node *elem_ptr = new_binary(ND_ADD, new_var_node(var, start), idx, start);
                    elem_ptr->ty = pointer_to(ty->base);
                    Node *deref = new_unary(ND_DEREF, elem_ptr, start);
                    deref->ty = ty->base;
                    Node *val = assign(&tok, tok);
                    add_type(val);
                    Node *asgn = new_binary(ND_ASSIGN, deref, val, start);
                    asgn->ty = ty->base;
                    result = new_binary(ND_COMMA, result, asgn, start);
                    result->ty = ty;
                    i++;
                    if (!equal(tok, "}"))
                        tok = skip(tok, ",");
                }
                tok = tok->next; // skip }
                // Final value is the array (decays to pointer)
                // Re-wrap so the last value is the variable itself
                Node *final_var = new_var_node(var, start);
                result = new_binary(ND_COMMA, result, final_var, start);
                add_type(result);
            } else if (ty->kind == TY_STRUCT || ty->kind == TY_UNION) {
                // Struct compound literal: assign each member
                Member *mem = ty->members;
                while (!equal(tok, "}") && mem) {
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
                    mem = mem->next;
                    if (!equal(tok, "}"))
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
                if (equal(tok, ",")) tok = tok->next;
                tok = skip(tok, "}");
                Node *asgn = new_binary(ND_ASSIGN, new_var_node(var, start), val, start);
                asgn->ty = ty;
                result = new_binary(ND_COMMA, asgn, new_var_node(var, start), start);
                add_type(result);
            }

            *rest = tok;
            return result;
        }

        Node *node = new_unary(ND_CAST, unary(rest, tok), start);
        node->ty = ty;
        return node;
    }
    return primary(rest, tok);
}

static Node *mul(Token **rest, Token *tok) {
    Node *node = unary(&tok, tok);
    for (;;) {
        Token *start = tok;
        if (equal(tok, "*")) {
            node = new_binary(ND_MUL, node, unary(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, "/")) {
            node = new_binary(ND_DIV, node, unary(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, "%")) {
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
        if (equal(tok, "+")) {
            node = new_binary(ND_ADD, node, mul(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, "-")) {
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
        if (equal(tok, "<<")) {
            node = new_binary(ND_SHL, node, add(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, ">>")) {
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
        if (equal(tok, "<")) {
            node = new_binary(ND_LT, node, shift(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, "<=")) {
            node = new_binary(ND_LE, node, shift(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, ">")) {
            node = new_binary(ND_LT, shift(&tok, tok->next), node, start);
            continue;
        }
        if (equal(tok, ">=")) {
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
        if (equal(tok, "==")) {
            node = new_binary(ND_EQ, node, relational(&tok, tok->next), start);
            continue;
        }
        if (equal(tok, "!=")) {
            node = new_binary(ND_NE, node, relational(&tok, tok->next), start);
            continue;
        }
        *rest = tok;
        return node;
    }
}

static Node *bitand(Token **rest, Token *tok) {
    Node *node = equality(&tok, tok);
    while (equal(tok, "&")) {
        Token *start = tok;
        node = new_binary(ND_BITAND, node, equality(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitxor(Token **rest, Token *tok) {
    Node *node = bitand(&tok, tok);
    while (equal(tok, "^")) {
        Token *start = tok;
        node = new_binary(ND_BITXOR, node, bitand(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *bitor(Token **rest, Token *tok) {
    Node *node = bitxor(&tok, tok);
    while (equal(tok, "|")) {
        Token *start = tok;
        node = new_binary(ND_BITOR, node, bitxor(&tok, tok->next), start);
    }
    *rest = tok;
    return node;
}

static Node *logand(Token **rest, Token *tok) {
    Node *node = bitor(&tok, tok);
    while (equal(tok, "&&"))
        node = new_binary(ND_LOGAND, node, bitor(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static Node *logor(Token **rest, Token *tok) {
    Node *node = logand(&tok, tok);
    while (equal(tok, "||"))
        node = new_binary(ND_LOGOR, node, logand(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static Node *conditional(Token **rest, Token *tok) {
    Node *node = logor(&tok, tok);
    if (equal(tok, "?")) {
        Node *cond = node;
        Node *then = expr(&tok, tok->next);
        tok = skip(tok, ":");
        Node *els = conditional(&tok, tok);
        node = new_node(ND_COND, cond->tok);
        node->cond = cond;
        node->then = then;
        node->els = els;
    }
    *rest = tok;
    return node;
}

static Node *assign(Token **rest, Token *tok) {
    Node *node = conditional(&tok, tok);
    if (equal(tok, "="))
        node = new_binary(ND_ASSIGN, node, assign(&tok, tok->next), tok);
    else if (equal(tok, "+="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_ADD, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "-="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SUB, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "*="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_MUL, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "/="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_DIV, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "%="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_MOD, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "&="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITAND, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "|="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITOR, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "^="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_BITXOR, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, "<<="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SHL, node, assign(&tok, tok->next), tok), tok);
    else if (equal(tok, ">>="))
        node = new_binary(ND_ASSIGN, node, new_binary(ND_SHR, node, assign(&tok, tok->next), tok), tok);
    *rest = tok;
    return node;
}

static Node *expr(Token **rest, Token *tok) {
    Node *node = assign(&tok, tok);
    while (equal(tok, ","))
        node = new_binary(ND_COMMA, node, assign(&tok, tok->next), tok);
    *rest = tok;
    return node;
}

static LVar *parse_params(Token **rest, Token *tok, bool *is_variadic) {
    LVar head = {};
    LVar *cur = &head;
    int param_index = 0;

    *is_variadic = false;
    if (equal(tok, "void") && equal(tok->next, ")")) {
        *rest = tok->next;
        return NULL;
    }

    while (!equal(tok, ")")) {
        if (cur != &head)
            tok = skip(tok, ",");
        if (equal(tok, "...")) {
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
        if (read_global_label_initializer(&tok, tok, &label)) {
            var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
            var->init_size = var->ty->size;
            append_reloc(var, 0, label, 0);
            *rest = tok;
            return;
        }
    }

    if (var->ty->kind == TY_ARRAY && var->ty->base->kind == TY_PTR && equal(tok, "{")) {
        int len = array_len(var->ty);
        if (len == 0) {
            Token *tmp = tok;
            len = count_array_initializer(&tmp, tmp);
            var->ty = array_of(var->ty->base, len);
        }

        int elem_size = var->ty->base->size;
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;

        tok = skip(tok, "{");
        int idx = 0;
        while (!equal(tok, "}")) {
            char *label = NULL;
            Token *next = tok;
            if (read_global_label_initializer(&next, tok, &label)) {
                if (idx < len)
                    append_reloc(var, idx * elem_size, label, 0);
                tok = next;
            } else {
                int val = read_const_initializer(&tok, tok);
                if (idx < len)
                    write_scalar_bytes(var->init_data, idx * elem_size, elem_size, val);
            }
            idx++;
            if (equal(tok, ",")) {
                tok = tok->next;
                if (equal(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        *rest = skip(tok, "}");
        return;
    }

    if (var->ty->kind == TY_ARRAY && equal(tok, "{") && is_integer(var->ty->base)) {
        int len = array_len(var->ty);
        if (len == 0) {
            Token *tmp = tok;
            len = count_array_initializer(&tmp, tmp);
            var->ty = array_of(var->ty->base, len);
        }

        int elem_size = var->ty->base->size;
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;

        tok = skip(tok, "{");
        int idx = 0;
        while (!equal(tok, "}")) {
            int val = read_const_initializer(&tok, tok);
            if (idx < len)
                write_scalar_bytes(var->init_data, idx * elem_size, elem_size, val);
            idx++;
            if (equal(tok, ",")) {
                tok = tok->next;
                if (equal(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        *rest = skip(tok, "}");
        return;
    }

    if ((var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) && equal(tok, "{")) {
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;
        tok = skip(tok, "{");
        Member *mem = var->ty->members;
        while (!equal(tok, "}")) {
            if (mem) {
                int64_t val = read_const_initializer(&tok, tok);
                write_scalar_bytes(var->init_data, mem->offset, mem->ty->size, val);
                mem = mem->next;
            } else {
                read_const_initializer(&tok, tok);
            }
            if (equal(tok, ",")) {
                tok = tok->next;
                if (equal(tok, "}"))
                    break;
                continue;
            }
            break;
        }
        *rest = skip(tok, "}");
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

Program *parse(Token *tok) {
    Function head = {};
    Function *fn_cur = &head;

    while (tok->kind != TK_EOF) {
        VarAttr attr = {};
        Type *base = declspec(&tok, tok, &attr);

        if (equal(tok, ";")) {
            tok = tok->next;
            continue;
        }

        char *name = NULL;
        Type *ty = declarator(&tok, tok, copy_type(base), &name);
        tok = skip_attributes(tok);

        if (!name) {
            tok = skip(tok, ";");
            continue;
        }

        if (equal(tok, "(")) {
            bool is_variadic = false;
            Type *fty = func_type(ty);
            Type *fn_symbol_ty = pointer_to(fty);
            locals = NULL;
            stack_offset = 64; // 48 (callee-saved pushes) + 16 (spill slots for r10/r11)

            tok = tok->next;
            LVar *params = parse_params(&tok, tok, &is_variadic);
            tok = skip(tok, ")");
            tok = skip_attributes(tok);

            // Build parameter type list
            fty->is_variadic = is_variadic;
            Type param_head = {};
            Type *pcur = &param_head;
            for (LVar *p = params; p; p = p->param_next) {
                Type *pt = arena_alloc(sizeof(Type));
                *pt = *p->ty;
                pt->param_next = NULL;
                pcur->param_next = pt;
                pcur = pt;
            }
            fty->param_types = param_head.param_next;

            // Register function symbol
            LVar *existing = find_global_name(name);
            if (!existing) {
                LVar *fn_sym = new_var(name, fn_symbol_ty, false);
                fn_sym->is_extern = true;
                fn_sym->is_function = true;
            } else {
                existing->ty = fn_symbol_ty;
            }

            if (equal(tok, ";")) {
                tok = tok->next;
                continue;
            }
            if (!equal(tok, "{"))
                error_tok(tok, "expected function body");

            Node *body = compound_stmt(&tok, tok);
            Function *fn = arena_alloc(sizeof(Function));
            fn->name = name;
            fn->ty = fty;
            fn->params = params;
            fn->body = body->body;
            fn->stack_size = align_to(stack_offset, 16);
            fn->is_variadic = is_variadic;
            fn_cur = fn_cur->next = fn;
            continue;
        }

        for (;;) {
            if (attr.is_typedef) {
                Typedef *td = arena_alloc(sizeof(Typedef));
                td->name = name;
                td->ty = ty;
                td->next = typedefs;
                typedefs = td;
            } else {
                if (equal(tok, "="))
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
                if (equal(tok, "=")) {
                    tok = tok->next;
                    global_initializer(&tok, tok, var);
                }
            }

            if (!equal(tok, ","))
                break;
            tok = tok->next;
            name = NULL;
            ty = declarator(&tok, tok, copy_type(base), &name);
            tok = skip_attributes(tok);
            if (!name)
                error_tok(tok, "expected variable name");
        }

        tok = skip(tok, ";");
    }

    Program *prog = arena_alloc(sizeof(Program));
    prog->funcs = head.next;
    prog->globals = globals;
    prog->strs = str_lits;
    return prog;
}
