#include "rcc.h"

Type *ty_void    = &(Type){.kind=TY_VOID,    .size=1,  .align=1};
Type *ty_bool    = &(Type){.kind=TY_BOOL,    .size=1,  .align=1,  .is_unsigned=true};
Type *ty_char    = &(Type){.kind=TY_CHAR,    .size=1,  .align=1};
Type *ty_uchar   = &(Type){.kind=TY_CHAR,    .size=1,  .align=1,  .is_unsigned=true};
Type *ty_short   = &(Type){.kind=TY_SHORT,   .size=2,  .align=2};
Type *ty_ushort  = &(Type){.kind=TY_SHORT,   .size=2,  .align=2,  .is_unsigned=true};
Type *ty_int     = &(Type){.kind=TY_INT,     .size=4,  .align=4};
Type *ty_uint    = &(Type){.kind=TY_INT,     .size=4,  .align=4,  .is_unsigned=true};
Type *ty_long    = &(Type){.kind=TY_LONG,    .size=4,  .align=4};
Type *ty_ulong   = &(Type){.kind=TY_LONG,    .size=4,  .align=4,  .is_unsigned=true};
Type *ty_llong   = &(Type){.kind=TY_LLONG,   .size=8,  .align=8};
Type *ty_ullong  = &(Type){.kind=TY_LLONG,   .size=8,  .align=8,  .is_unsigned=true};
Type *ty_float   = &(Type){.kind=TY_FLOAT,   .size=4,  .align=4};
Type *ty_double  = &(Type){.kind=TY_DOUBLE,  .size=8,  .align=8};
Type *ty_ldouble = &(Type){.kind=TY_LDOUBLE, .size=16, .align=16};

bool is_integer(Type *ty) {
    return ty->kind == TY_BOOL || ty->kind == TY_CHAR || ty->kind == TY_SHORT ||
           ty->kind == TY_INT || ty->kind == TY_LONG || ty->kind == TY_LLONG;
}

bool is_flonum(Type *ty) {
    return ty->kind == TY_FLOAT || ty->kind == TY_DOUBLE || ty->kind == TY_LDOUBLE;
}

bool is_number(Type *ty) {
    return is_integer(ty) || is_flonum(ty);
}

Type *get_integer_type(int size, bool is_unsigned) {
    if (size <= 1)
        return is_unsigned ? ty_uchar : ty_char;
    if (size <= 2)
        return is_unsigned ? ty_ushort : ty_short;
    if (size <= 4)
        return is_unsigned ? ty_uint : ty_int;
    return is_unsigned ? ty_ullong : ty_llong;
}

Type *pointer_to(Type *base) {
    Type *ty = arena_alloc(sizeof(Type));
    ty->kind = TY_PTR;
    ty->size = 8;
    ty->align = 8;
    ty->base = base;
    return ty;
}

Type *array_of(Type *base, int len) {
    Type *ty = arena_alloc(sizeof(Type));
    ty->kind = TY_ARRAY;
    ty->size = base->size * len;
    ty->align = base->align;
    ty->base = base;
    return ty;
}

static Node *new_scale_mul(Node *rhs, int size) {
    Node *num = arena_alloc(sizeof(Node));
    num->kind = ND_NUM;
    num->val = size;
    num->ty = ty_int;
    Node *node = arena_alloc(sizeof(Node));
    node->kind = ND_MUL;
    node->lhs = rhs;
    node->rhs = num;
    node->ty = ty_int;
    return node;
}

static Type *integer_promotion(Type *ty) {
    if (!is_integer(ty))
        return ty;
    if (ty->size < 4)
        return ty_int;
    return ty;
}

static Type *get_float_type(Type *lhs, Type *rhs) {
    if (lhs->kind == TY_LDOUBLE || rhs->kind == TY_LDOUBLE)
        return ty_ldouble;
    if (lhs->kind == TY_DOUBLE || rhs->kind == TY_DOUBLE)
        return ty_double;
    return ty_float;
}

static Type *usual_arith_type(Type *lhs, Type *rhs) {
    if (is_flonum(lhs) || is_flonum(rhs))
        return get_float_type(lhs, rhs);
    lhs = integer_promotion(lhs);
    rhs = integer_promotion(rhs);
    int size = lhs->size > rhs->size ? lhs->size : rhs->size;
    if (size < 4)
        size = 4;
    return get_integer_type(size, lhs->is_unsigned || rhs->is_unsigned);
}

static void add_type_internal(Node *node) {
    if (!node || node->ty) return;

    add_type_internal(node->lhs);
    add_type_internal(node->rhs);
    add_type_internal(node->cond);
    add_type_internal(node->then);
    add_type_internal(node->els);
    add_type_internal(node->init);
    add_type_internal(node->inc);
    add_type_internal(node->case_next);
    add_type_internal(node->default_case);

    for (Node *n = node->body; n; n = n->next)
        add_type_internal(n);
    for (Node *n = node->args; n; n = n->next)
        add_type_internal(n);

    switch (node->kind) {
    case ND_ADD:
    case ND_SUB: {
        Type *lty = node->lhs->ty;
        Type *rty = node->rhs->ty;
        if (is_number(lty) && is_number(rty)) {
            node->ty = usual_arith_type(lty, rty);
            return;
        }
        if (lty->base && is_integer(rty)) {
            node->rhs = new_scale_mul(node->rhs, lty->base->size);
            node->ty = (lty->kind == TY_ARRAY) ? pointer_to(lty->base) : lty;
            return;
        }
        if (is_integer(lty) && rty->base) {
            // ptr + int
            if (node->kind == ND_SUB) {
                // error: int - ptr is invalid
                node->ty = ty_int; // fallback
                return;
            }
            Node *tmp = node->lhs;
            node->lhs = node->rhs;
            node->rhs = tmp;
            node->rhs = new_scale_mul(node->rhs, rty->base->size);
            node->ty = (rty->kind == TY_ARRAY) ? pointer_to(rty->base) : rty;
            return;
        }
        if (lty->base && rty->base) {
            // ptr - ptr
            // For now just output int
            node->ty = ty_int;
            return;
        }
        node->ty = ty_int;
        return;
    }
    case ND_MUL:
    case ND_DIV:
    case ND_MOD:
    case ND_BITAND:
    case ND_BITXOR:
    case ND_BITOR:
        node->ty = usual_arith_type(node->lhs->ty, node->rhs->ty);
        return;
    case ND_NEG:
        node->ty = is_flonum(node->lhs->ty) ? node->lhs->ty : integer_promotion(node->lhs->ty);
        return;
    case ND_NOT:
        node->ty = ty_int;
        return;
    case ND_FNUM:
        return;
    case ND_SHL:
    case ND_SHR:
        node->ty = integer_promotion(node->lhs->ty);
        return;
    case ND_LOGAND:
    case ND_LOGOR:
        node->ty = ty_int;
        return;
    case ND_ASSIGN:
        if (node->lhs->ty && node->rhs->ty) {
            bool lf = is_flonum(node->lhs->ty);
            bool rf = is_flonum(node->rhs->ty);
            if ((lf && !rf) || (!lf && rf) ||
                (lf && rf && node->lhs->ty->size != node->rhs->ty->size)) {
                Node *cast = arena_alloc(sizeof(Node));
                cast->kind = ND_CAST;
                cast->lhs = node->rhs;
                cast->ty = node->lhs->ty;
                cast->tok = node->rhs->tok;
                node->rhs = cast;
            }
        }
        node->ty = node->lhs->ty;
        return;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE:
    case ND_SIZEOF:
        node->ty = ty_int;
        return;
    case ND_POST_INC:
    case ND_POST_DEC:
        node->ty = node->lhs->ty;
        return;
    case ND_COND:
        node->ty = node->then->ty ? node->then->ty : node->els->ty;
        return;
    case ND_COMMA:
        node->ty = node->rhs->ty;
        return;
    case ND_NUM:
        node->ty = ty_int;
        return;
    case ND_LVAR:
        if (!node->var->ty) node->var->ty = ty_int;
        node->ty = node->var->ty;
        return;
    case ND_ADDR:
        node->ty = pointer_to(node->lhs->ty);
        return;
    case ND_DEREF:
        if (node->lhs->ty->kind != TY_PTR && node->lhs->ty->kind != TY_ARRAY) {
            error_tok(node->tok, "invalid pointer dereference\n\033[1;36mnote\033[0m: cannot apply '*' to a non-pointer type");
        }
        node->ty = node->lhs->ty->base;
        return;
    case ND_CAST:
        if (!node->ty)
            node->ty = node->lhs->ty;
        return;
    case ND_BITNOT:
        node->ty = integer_promotion(node->lhs->ty);
        return;
    case ND_FUNCALL:
        if (node->lhs && node->lhs->ty && node->lhs->ty->kind == TY_PTR &&
            node->lhs->ty->base && node->lhs->ty->base->kind == TY_FUNC) {
            node->ty = node->lhs->ty->base->return_ty;
        } else if (node->funcname) {
            node->ty = ty_int;
        } else {
            node->ty = ty_int;
        }
        for (Node *n = node->args; n; n = n->next)
            add_type(n);
        return;
    case ND_STR:
        node->ty = pointer_to(ty_char);
        return;
    case ND_MEMBER:
        node->ty = node->member->ty;
        return;
    case ND_STMT_EXPR: {
        Node *last = node->body;
        while (last && last->next)
            last = last->next;
        if (last && last->kind == ND_EXPR_STMT && last->lhs) {
            add_type_internal(last->lhs);
            node->ty = last->lhs->ty;
        } else {
            node->ty = ty_int;
        }
        return;
    }
    case ND_DO:
    case ND_SWITCH:
    case ND_CASE:
    case ND_BREAK:
    case ND_CONTINUE:
    case ND_GOTO:
    case ND_LABEL:
    case ND_NULL:
        return;
    default:
        return;
    }
}

void add_type(Node *node) {
    add_type_internal(node);
}
