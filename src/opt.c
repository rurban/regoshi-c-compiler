#include "rcc.h"
#include <string.h>

#define CF_NEXT 0
#define CF_RETURN 1

static int eval_ast(Program *prog, Function *fn, Node *node, int *env, int *cf, bool *success);

static int eval_ast(Program *prog, Function *fn, Node *node, int *env, int *cf, bool *success) {
    if (!node || !*success) return 0;

    switch (node->kind) {
    case ND_NUM:
        return node->val;
    case ND_LVAR:
        return env[node->var->offset / 8];
    case ND_ASSIGN: {
        if (node->lhs->kind != ND_LVAR) {
            *success = false;
            return 0;
        }
        int dummy_cf = CF_NEXT;
        int val = eval_ast(prog, fn, node->rhs, env, &dummy_cf, success);
        env[node->lhs->var->offset / 8] = val;
        return val;
    }
    case ND_BLOCK:
        for (Node *n = node->body; n; n = n->next) {
            int ret = eval_ast(prog, fn, n, env, cf, success);
            if (!*success || *cf == CF_RETURN) return ret;
        }
        return 0;
    case ND_RETURN: {
        int dummy_cf = CF_NEXT;
        int ret = eval_ast(prog, fn, node->lhs, env, &dummy_cf, success);
        *cf = CF_RETURN;
        return ret;
    }
    case ND_IF: {
        int dummy_cf = CF_NEXT;
        int cond = eval_ast(prog, fn, node->cond, env, &dummy_cf, success);
        if (!*success) return 0;
        if (cond) {
            return eval_ast(prog, fn, node->then, env, cf, success);
        } else if (node->els) {
            return eval_ast(prog, fn, node->els, env, cf, success);
        }
        return 0;
    }
    case ND_EXPR_STMT: {
        int dummy_cf = CF_NEXT;
        return eval_ast(prog, fn, node->lhs, env, &dummy_cf, success);
    }
    case ND_ADD:
    case ND_SUB:
    case ND_MUL:
    case ND_DIV:
    case ND_MOD:
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE: {
        int dummy_cf = CF_NEXT;
        int l = eval_ast(prog, fn, node->lhs, env, &dummy_cf, success);
        int r = eval_ast(prog, fn, node->rhs, env, &dummy_cf, success);
        if (!*success) return 0;
        if (node->kind == ND_ADD) return l + r;
        if (node->kind == ND_SUB) return l - r;
        if (node->kind == ND_MUL) return l * r;
        if (node->kind == ND_DIV) {
            if (r == 0) { *success = false; return 0; }
            return l / r;
        }
        if (node->kind == ND_MOD) {
            if (r == 0) { *success = false; return 0; }
            return l % r;
        }
        if (node->kind == ND_EQ) return l == r;
        if (node->kind == ND_NE) return l != r;
        if (node->kind == ND_LT) return l < r;
        if (node->kind == ND_LE) return l <= r;
        *success = false;
        return 0;
    }
    case ND_FUNCALL: {
        int args[10];
        int nargs = 0;
        for (Node *arg = node->args; arg; arg = arg->next) {
            int dummy_cf = CF_NEXT;
            args[nargs++] = eval_ast(prog, fn, arg, env, &dummy_cf, success);
            if (!*success || nargs >= 10) {
                *success = false; return 0;
            }
        }
        Function *target = NULL;
        for (Function *f = prog->funcs; f; f = f->next) {
            if (strcmp(f->name, node->funcname) == 0) {
                target = f; break;
            }
        }
        if (target && target->body && strcmp(target->name, "printf") != 0) {
            int new_env[256] = {0};
            LVar *param = target->params;
            for (int i=0; i<nargs; i++) {
                if (param) {
                    new_env[param->offset / 8] = args[i];
                    param = param->param_next;
                }
            }
            int cf_ret = CF_NEXT;
            int ret = 0;
            for (Node *stmt = target->body; stmt; stmt = stmt->next) {
                ret = eval_ast(prog, target, stmt, new_env, &cf_ret, success);
                if (!*success || cf_ret == CF_RETURN) break;
            }
            return ret;
        }
        *success = false;
        return 0;
    }
    default:
        *success = false;
        return 0;
    }
}

static bool has_cleanup_local(Function *fn) {
    for (LVar *var = fn->locals; var; var = var->next) {
        if (var->cleanup_func)
            return true;
    }
    return false;
}

static bool has_addr_arg(Node *node) {
    for (Node *arg = node->args; arg; arg = arg->next) {
        if (arg->kind == ND_ADDR)
            return true;
    }
    return false;
}

static Node *optimize_node(Program *prog, Node *node) {
    if (!node) return NULL;
    node->lhs = optimize_node(prog, node->lhs);
    node->rhs = optimize_node(prog, node->rhs);
    node->cond = optimize_node(prog, node->cond);
    node->then = optimize_node(prog, node->then);
    node->els = optimize_node(prog, node->els);
    node->init = optimize_node(prog, node->init);
    node->inc = optimize_node(prog, node->inc);
    
    // We can't easily map node->next without breaking lists potentially? 
    // Wait, body is a list. args is a list.
    Node *prev_body = NULL;
    for (Node *n = node->body; n; n = n->next) {
        Node *o = optimize_node(prog, n);
        if (prev_body) prev_body->next = o;
        else node->body = o;
        prev_body = o;
    }
    Node *prev_arg = NULL;
    for (Node *n = node->args; n; n = n->next) {
        Node *o = optimize_node(prog, n);
        if (prev_arg) prev_arg->next = o;
        else node->args = o;
        prev_arg = o;
    }

    if (node->kind == ND_ADD || node->kind == ND_SUB || node->kind == ND_MUL || node->kind == ND_DIV || node->kind == ND_MOD) {
        if (node->lhs && node->lhs->kind == ND_NUM && node->rhs && node->rhs->kind == ND_NUM) {
            Node *fold = arena_alloc(sizeof(Node));
            fold->kind = ND_NUM;
            if (node->kind == ND_ADD) fold->val = node->lhs->val + node->rhs->val;
            if (node->kind == ND_SUB) fold->val = node->lhs->val - node->rhs->val;
            if (node->kind == ND_MUL) fold->val = node->lhs->val * node->rhs->val;
            if (node->kind == ND_DIV) {
                if (node->rhs->val == 0) return node; // avoid div by zero
                fold->val = node->lhs->val / node->rhs->val;
            }
            if (node->kind == ND_MOD) {
                if (node->rhs->val == 0) return node;
                fold->val = node->lhs->val % node->rhs->val;
            }
            fold->ty = node->ty;
            return fold;
        }
    }
    
    if (node->kind == ND_FUNCALL && node->funcname) {
        bool all_const = true;
        int args[10];
        int nargs = 0;
        for (Node *arg = node->args; arg; arg = arg->next) {
            if (arg->kind != ND_NUM) {
                all_const = false; break;
            }
            if (nargs < 10) args[nargs++] = arg->val;
        }
        if (all_const && !has_addr_arg(node) && strcmp(node->funcname, "printf") != 0) {
            Function *target = NULL;
            for (Function *fn = prog->funcs; fn; fn = fn->next) {
                if (strcmp(fn->name, node->funcname) == 0) {
                    target = fn; break;
                }
            }
            if (target && target->body && !has_cleanup_local(target)) {
                bool success = true;
                int env[256] = {0}; // simple addressing
                LVar *param = target->params;
                for (int i=0; i<nargs; i++) {
                    if (param) {
                        env[param->offset / 8] = args[i];
                        param = param->param_next;
                    }
                }
                int dummy_cf = CF_NEXT;
                int result = 0;
                for (Node *stmt = target->body; stmt; stmt = stmt->next) {
                    result = eval_ast(prog, target, stmt, env, &dummy_cf, &success);
                    if (!success || dummy_cf == CF_RETURN) break;
                }
                if (success) {
                    Node *fold = arena_alloc(sizeof(Node));
                    fold->kind = ND_NUM;
                    fold->val = result;
                    fold->ty = node->ty; // assume integer
                    return fold;
                }
            }
        }
    }

    return node;
}

void optimize(Program *prog) {
    for (Function *fn = prog->funcs; fn; fn = fn->next) {
        Node *prev = NULL;
        for (Node *n = fn->body; n; n = n->next) {
            Node *o = optimize_node(prog, n);
            if (prev) prev->next = o;
            else fn->body = o;
            prev = o;
        }
    }
}
