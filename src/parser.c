#include "rcc.h"

typedef struct VarAttr VarAttr;
typedef struct TagScope TagScope;
typedef struct EnumConst EnumConst;

struct VarAttr {
    bool is_typedef;
    bool is_extern;
    bool is_static;
    bool has_type;
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

static StrLit *str_lits;
static int str_lit_counter;

static Node *current_switch;
static Node *current_loop;
static int static_local_counter;
static LVar *current_fn_scope_locals;
static int current_block_depth;
static bool suppress_fn_scope_update;

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
    int len = (int)strlen(op);
    return tok->len == len && memcmp(tok->loc, op, len) == 0;
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
        node->ty = ty_llong; // On LLP64, values > INT_MAX go to llong
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

static Member *find_member(Type *ty, Token *tok) {
    if (ty->kind != TY_STRUCT && ty->kind != TY_UNION)
        error_tok(tok, "not a struct or union");
    for (Member *mem = ty->members; mem; mem = mem->next)
        if (equal(tok, mem->name))
            return mem;
    return NULL;
}

static StrLit *new_str_lit(char *str, int prefix, int elem_size) {
    StrLit *s = arena_alloc(sizeof(StrLit));
    s->str = str;
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
    }

    if (!head.next)
        return body;

    Node *node = new_node(ND_BLOCK, tok);
    node->body = head.next;
    return node;
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

        if (equal(tok, "__asm__") || equal(tok, "__asm")) {
            tok = tok->next;
            tok = skip(tok, "(");
            while (tok->kind == TK_STR || (tok->kind == TK_IDENT && equal(tok, "_"))) {
                tok = tok->next;
            }
            tok = skip(tok, ")");
            continue;
        }

        if (equal(tok, "__attribute__") || equal(tok, "__attribute")) {
            tok = tok->next;
            tok = skip(tok, "(");
            tok = skip(tok, "(");
            while (!(equal(tok, ")") && equal(tok->next, ")"))) {
                if (equal(tok, "__cleanup__") || equal(tok, "cleanup")) {
                    pending_cleanup_tok = tok;
                    tok = tok->next;
                    tok = skip(tok, "(");
                    if (tok->kind == TK_IDENT)
                        pending_cleanup_func = tok->name;
                    tok = tok->next;
                    tok = skip(tok, ")");
                    if (equal(tok, ","))
                        tok = tok->next;
                    continue;
                }

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
    int bit_pos = 0; // current bit position within the struct (for bitfield packing)
    int bf_unit_size = 0; // size of current bitfield storage unit (0 = none active)
    int struct_pack = pack_align; // capture #pragma pack value at struct start

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

            // Check for bitfield
            int bit_width = 0;
            if (equal(tok, ":")) {
                tok = tok->next;
                Node *width_node = conditional(&tok, tok);
                long long w;
                if (!eval_const_expr(width_node, &w))
                    error_tok(tok, "bitfield width must be a constant expression");
                bit_width = (int)w;
                if (bit_width < 0 || bit_width > mem_ty->size * 8)
                    error_tok(tok, "bitfield width out of range");
            }

            // Handle anonymous bitfield: "int : N" or "int : 0"
            // These don't create named members but affect layout
            if (!name && bit_width >= 0) {
                if (!is_union) {
                    int unit = mem_ty->size;
                    int unit_bits = unit * 8;
                    // :0 always forces a new storage unit
                    if (bit_width == 0 || bf_unit_size != unit ||
                        bit_pos % unit_bits + bit_width > unit_bits) {
                        offset = align_to(offset, unit);
                        bit_pos = offset * 8;
                        bf_unit_size = unit;
                    }
                    if (bit_width > 0) {
                        bit_pos += bit_width;
                        int end_byte = (bit_pos + 7) / 8;
                        if (end_byte > offset)
                            offset = end_byte;
                    }
                }
                if (!equal(tok, ","))
                    break;
                tok = tok->next;
                continue;
            }

            if (!name)
                error_tok(tok, "expected member name");

            Member *mem = arena_alloc(sizeof(Member));
            mem->name = name;
            mem->bit_width = bit_width;

            if (bit_width > 0) {
                // Bitfield member: pack within a storage unit of mem_ty->size
                int unit = mem_ty->size; // storage unit size in bytes
                int unit_bits = unit * 8;

                // Check if we need to start a new storage unit:
                // - no current unit, or different unit size, or doesn't fit
                if (bf_unit_size != unit ||
                    bit_pos % (unit * 8) + bit_width > unit_bits) {
                    // Align to start of a new storage unit
                    if (!is_union) {
                        int a = unit;
                        if (struct_pack > 0 && (struct_pack < a || a == 0))
                            a = struct_pack;
                        offset = align_to(offset, a);
                        bit_pos = offset * 8;
                    }
                    bf_unit_size = unit;
                }

                mem->ty = mem_ty;
                mem->bit_offset = bit_pos % unit_bits;
                if (is_union) {
                    mem->offset = 0;
                    if (max_size < unit)
                        max_size = unit;
                } else {
                    mem->offset = (bit_pos / unit_bits) * unit;
                    bit_pos += bit_width;
                    int end_byte = (bit_pos + 7) / 8;
                    if (end_byte > offset)
                        offset = end_byte;
                }
                if (max_align < unit)
                    max_align = unit;
            } else {
                // Normal (non-bitfield) member
                bf_unit_size = 0; // end any bitfield packing run
                mem->ty = mem_ty;
                mem->bit_offset = 0;
                if (is_union) {
                    mem->offset = 0;
                    if (max_size < mem_ty->size)
                        max_size = mem_ty->size;
                } else {
                    int a = mem_ty->align;
                    if (struct_pack > 0 && (struct_pack < a || a == 0))
                        a = struct_pack;
                    offset = align_to(offset, a);
                    mem->offset = offset;
                    offset += mem_ty->size;
                    bit_pos = offset * 8;
                    if (max_align < a)
                        max_align = a;
                }
                if (struct_pack > 0 && ty->pack_align == 0)
                    ty->pack_align = struct_pack;
            }
            if (name)
                cur = cur->next = mem;

            if (!equal(tok, ","))
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
            warn_tok(tok, "type defaults to int");
        }
    }

    if (!ty)
        error_tok(tok, "expected type name, got kind=%d text='%.20s'", tok->kind, tok->loc);

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
        StrLit *s = new_str_lit(tok->str, tok->string_literal_prefix, 1);
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

static Type *infer_array_type(Type *ty, Token *tok) {
    if (!ty || ty->kind != TY_ARRAY || ty->size != 0)
        return ty;
    if (tok->kind == TK_STR) {
        if (tok->string_literal_prefix == 0)
            return array_of(ty->base, (int)strlen(tok->str) + 1);
        // For wide strings, count UTF-8 characters (each becomes one wchar)
        return array_of(ty->base, utf8_len(tok->str) + 1);
    }
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
    // Skip empty nested initializer (e.g., for flexible array members)
    if (equal(tok, "{")) {
        *rest = skip_initializer(tok);
        return 0;
    }
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
    pending_cleanup_func = NULL;
    Type *base = declspec(&tok, tok, &attr);
    char *type_level_cleanup = pending_cleanup_func;
    Node head = {};
    Node *cur = &head;

    if (equal(tok, ";")) {
        pending_cleanup_func = NULL;
        *rest = tok->next;
        return new_node(ND_NULL, tok);
    }

    while (!equal(tok, ";")) {
        char *name = NULL;
        pending_cleanup_func = NULL;
        Type *ty = declarator(&tok, tok, copy_type(base), &name);
        tok = skip_attributes(tok);
        char *cleanup = pending_cleanup_func ? pending_cleanup_func : type_level_cleanup;
        pending_cleanup_func = NULL;
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
            add_typedef(name, ty);
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
            var->cleanup_func = cleanup ? cleanup : ty->cleanup_func;
            if (current_block_depth == 1)
                current_fn_scope_locals = locals;
            if (equal(tok, "=")) {
                Token *start = tok;
                tok = tok->next;
                if (var->ty->kind == TY_ARRAY) {
                    if (equal(tok, "{")) {
                        cur->next = local_array_initializer(&tok, tok, var);
                        while (cur->next)
                            cur = cur->next;
                    } else if (tok->kind == TK_STR) {
                        Node *lhs = new_var_node(var, start);
                        Node *rhs = assign(&tok, tok);
                        cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                    } else {
                        Node *lhs = new_var_node(var, start);
                        Node *rhs = assign(&tok, tok);
                        cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                    }
                } else {
                    // Handle struct/union initializer: { val1, val2, ... }
                    if (var->ty->kind == TY_STRUCT || var->ty->kind == TY_UNION) {
                        Node *lhs = new_var_node(var, start);
                        if (equal(tok, "{")) {
                            // Struct initializer - create member assignments
                            tok = tok->next;
                            Member *m = var->ty->members;
                            while (!equal(tok, "}") && m) {
                                // Handle nested struct initializers: { {a,b}, {c,d} }
                                if (equal(tok, "{") && m->ty &&
                                    (m->ty->kind == TY_STRUCT || m->ty->kind == TY_UNION)) {
                                    // Create a temporary lvar for the member
                                    LVar *mem_var = arena_alloc(sizeof(LVar));
                                    mem_var->name = "";
                                    mem_var->ty = m->ty;
                                    mem_var->is_local = true;

                                    // Recursively initialize the nested struct
                                    Node *mem_lhs = new_var_node(mem_var, tok);
                                    tok = tok->next;
                                    Member *nm = m->ty->members;
                                    while (!equal(tok, "}") && nm) {
                                        if (!equal(tok, ",")) {
                                            Node *rhs = assign(&tok, tok);
                                            Node *mem = new_node(ND_MEMBER, tok);
                                            mem->lhs = mem_lhs;
                                            mem->member = nm;
                                            cur = cur->next = new_unary(ND_EXPR_STMT,
                                                                        new_binary(ND_ASSIGN, mem, rhs, tok), tok);
                                        }
                                        if (equal(tok, ","))
                                            tok = tok->next;
                                        nm = nm->next;
                                    }
                                    tok = skip(tok, "}");
                                } else if (!equal(tok, ",")) {
                                    Node *rhs = assign(&tok, tok);
                                    Node *mem = new_node(ND_MEMBER, tok);
                                    mem->lhs = lhs;
                                    mem->member = m;
                                    cur = cur->next = new_unary(ND_EXPR_STMT,
                                                                new_binary(ND_ASSIGN, mem, rhs, tok), tok);
                                }
                                if (equal(tok, ","))
                                    tok = tok->next;
                                m = m->next;
                            }
                            tok = skip(tok, "}");
                        } else {
                            Node *rhs = assign(&tok, tok);
                            cur = cur->next = new_unary(ND_EXPR_STMT,
                                                        new_binary(ND_ASSIGN, lhs, rhs, start), start);
                        }
                    } else {
                        Node *lhs = new_var_node(var, start);
                        Node *rhs = assign(&tok, tok);
                        cur = cur->next = new_unary(ND_EXPR_STMT, new_binary(ND_ASSIGN, lhs, rhs, start), start);
                    }
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

static Node *stmt(Token **rest, Token *tok) {
    if (equal(tok, "return")) {
        Node *node = new_node(ND_RETURN, tok);
        node->cleanup_begin = locals;
        node->cleanup_end = current_fn_scope_locals;
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
        Node *saved_loop = current_loop;
        node->cleanup_end = locals;
        node->continue_cleanup_end = locals;
        current_loop = node;
        node->then = stmt(&tok, tok);
        current_loop = saved_loop;
        *rest = tok;
        return node;
    }

    if (equal(tok, "do")) {
        Node *node = new_node(ND_DO, tok);
        Node *saved_loop = current_loop;
        node->cleanup_end = locals;
        node->continue_cleanup_end = locals;
        current_loop = node;
        node->then = stmt(&tok, tok->next);
        current_loop = saved_loop;
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

        LVar *for_init_locals = locals;

        if (!equal(tok, ")"))
            node->inc = expr(&tok, tok);
        tok = skip(tok, ")");
        Node *saved_loop = current_loop;
        node->cleanup_end = for_init_locals;
        node->continue_cleanup_end = for_init_locals;
        current_loop = node;
        node->then = stmt(&tok, tok);
        current_loop = saved_loop;
        node = append_cleanup_range(node, locals, saved_locals, tok);
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
        node->cleanup_end = locals;
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

    if (equal(tok, "continue")) {
        Node *node = new_node(ND_CONTINUE, tok);
        node->cleanup_begin = locals;
        *rest = skip(tok->next, ";");
        if (!current_loop)
            error_tok(tok, "stray continue");
        node->cleanup_end = current_loop->continue_cleanup_end;
        return node;
    }

    if (equal(tok, "goto")) {
        Node *node = new_node(ND_GOTO, tok);
        tok = tok->next;
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

    if (tok->kind == TK_IDENT && equal(tok->next, ":")) {
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
            Node *last = node->body;
            while (last && last->next)
                last = last->next;
            if (last && last->kind == ND_EXPR_STMT && last->lhs)
                node->stmt_expr_result = last->lhs;
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
            else {
                node->funcname = tok->name;
                LVar *gvar = find_global_name(tok->name);
                if (gvar && gvar->is_function)
                    node->lhs = new_var_node(gvar, tok);
            }
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
        StrLit *s = new_str_lit(tok->str, tok->string_literal_prefix, node->ty->base->size);
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
                    else if (equal(tmp, "}"))
                        depth--;
                    else if (depth == 0 && equal(tmp, ","))
                        count++;
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
                    Node *deref = new_unary(ND_DEREF, elem_ptr, start);
                    Node *val = assign(&tok, tok);
                    add_type(val);
                    Node *asgn = new_binary(ND_ASSIGN, deref, val, start);
                    add_type(asgn);
                    result = new_binary(ND_COMMA, result, asgn, start);
                    add_type(result);
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

    if (var->ty->kind == TY_ARRAY && (var->ty->base->kind == TY_STRUCT || var->ty->base->kind == TY_UNION) && equal(tok, "{")) {
        int elem_size = var->ty->base->size;
        int len = array_len(var->ty);
        if (len == 0) {
            Token *tmp = tok;
            len = count_array_initializer(&tmp, tmp);
            var->ty = array_of(var->ty->base, len);
        }
        var->init_data = arena_alloc(var->ty->size ? var->ty->size : 1);
        var->init_size = var->ty->size;
        tok = skip(tok, "{");
        int idx = 0;
        while (!equal(tok, "}")) {
            if (equal(tok, "{")) {
                tok = skip(tok, "{");
                Member *mem = var->ty->base->members;
                while (!equal(tok, "}")) {
                    if (mem) {
                        int64_t val = read_const_initializer(&tok, tok);
                        if (idx < len)
                            write_scalar_bytes(var->init_data, idx * elem_size + mem->offset, mem->ty->size, val);
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
                tok = skip(tok, "}");
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
            stack_offset = 80; // reserve locals plus spill slots below rbp for r10/r11 saves
            label_scopes = NULL;
            pending_gotos = NULL;
            current_switch = NULL;
            current_loop = NULL;

            tok = tok->next;
            LVar *params = parse_params(&tok, tok, &is_variadic);
            tok = skip(tok, ")");
            tok = skip_attributes(tok);
            current_fn_scope_locals = params;
            current_block_depth = 0;
            suppress_fn_scope_update = false;

            if (fty->return_ty && (fty->return_ty->kind == TY_STRUCT || fty->return_ty->kind == TY_UNION)) {
                LVar *retbuf = new_var("", pointer_to(fty->return_ty), true);
                retbuf->cleanup_func = NULL;
            }

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

            // For typedefs like 'typedef int functype(int);', register the type
            if (attr.is_typedef) {
                add_typedef(name, fty);
                if (equal(tok, ";")) {
                    tok = tok->next;
                    continue;
                }
                error_tok(tok, "expected ';' after typedef");
            }

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
            fn->asm_name = format(".Lfn.%s", name);
            fn->ty = fty;
            fn->params = params;
            fn->locals = fn_locals;
            fn->body = body->body;
            fn->stack_size = align_to(stack_offset, 16);
            fn->is_variadic = is_variadic;
            fn_cur = fn_cur->next = fn;
            current_fn_scope_locals = NULL;
            current_block_depth = 0;
            suppress_fn_scope_update = false;
            continue;
        }

        for (;;) {
            if (attr.is_typedef) {
                add_typedef(name, ty);
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
