#include "rcc.h"
#include <ctype.h>
#include <stdlib.h>

typedef struct Macro Macro;
typedef struct OnceFile OnceFile;
typedef struct CondIncl CondIncl;

struct Macro {
    Macro *next;
    char *name;
    bool is_function;
    char **params;
    int param_len;
    char *body;
};

struct OnceFile {
    OnceFile *next;
    char *path;
};

struct CondIncl {
    CondIncl *next;
    bool parent_active;
    bool active;
    bool branch_taken;
};

typedef struct {
    char *buf;
    int len;
    int cap;
} StrBuf;

static Macro *macros;
static OnceFile *once_files;
static int pp_counter;

static bool pp_startswith(char *p, char *q) {
    return strncmp(p, q, strlen(q)) == 0;
}

static bool pp_is_ident1(char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_' || c == '$';
}

static bool pp_is_ident2(char c) {
    return pp_is_ident1(c) || ('0' <= c && c <= '9');
}

static char *pp_strndup(char *p, int len) {
    char *s = arena_alloc(len + 1);
    memcpy(s, p, len);
    s[len] = '\0';
    return s;
}

static void sb_init(StrBuf *sb, int cap) {
    sb->buf = arena_alloc(cap);
    sb->len = 0;
    sb->cap = cap;
    sb->buf[0] = '\0';
}

static void sb_reserve(StrBuf *sb, int needed) {
    if (needed <= sb->cap)
        return;

    int new_cap = sb->cap;
    while (new_cap < needed)
        new_cap *= 2;

    char *new_buf = arena_alloc(new_cap);
    memcpy(new_buf, sb->buf, sb->len + 1);
    sb->buf = new_buf;
    sb->cap = new_cap;
}

static void sb_putc(StrBuf *sb, char c) {
    sb_reserve(sb, sb->len + 2);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static void sb_puts(StrBuf *sb, char *s) {
    int len = strlen(s);
    sb_reserve(sb, sb->len + len + 1);
    memcpy(sb->buf + sb->len, s, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

static char *splice_lines(char *input) {
    int len = strlen(input);
    char *buf = arena_alloc(len + 1);
    int j = 0;

    for (int i = 0; i < len; i++) {
        if (input[i] == '\\' && input[i + 1] == '\n') {
            i++;
            continue;
        }
        buf[j++] = input[i];
    }

    buf[j] = '\0';
    return buf;
}

static char *path_dirname(char *path) {
    char *last = path;
    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return pp_strndup(path, last - path);
}

static char *path_basename(char *path) {
    char *last = path;
    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
    }
    return last;
}

static char *path_join(char *dir, char *file) {
    if (!*dir)
        return str_intern(file, strlen(file));
    return format("%s%s%s", dir, (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : "\\", file);
}

static bool file_exists(char *path) {
    FILE *fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        return true;
    }
    return false;
}

static char *canonical_path(char *path) {
    char full[4096];
#ifdef _WIN32
    if (_fullpath(full, path, sizeof(full)))
        return str_intern(full, strlen(full));
#else
    if (realpath(path, full))
        return str_intern(full, strlen(full));
#endif
    return str_intern(path, strlen(path));
}

static char *read_pp_file(char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;

    int cap = 10 * 1024 * 1024;
    char *buf = arena_alloc(cap);
    int size = fread(buf, 1, cap - 2, fp);
    fclose(fp);
    if (size == 0 || buf[size - 1] != '\n')
        buf[size++] = '\n';
    buf[size] = '\0';
    return buf;
}

static Macro *find_macro(char *name) {
    for (Macro *m = macros; m; m = m->next)
        if (strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static void define_macro(char *name, bool is_function, char **params, int param_len, char *body) {
    Macro *m = find_macro(name);
    if (!m) {
        m = arena_alloc(sizeof(Macro));
        m->next = macros;
        macros = m;
    }
    m->name = name;
    m->is_function = is_function;
    m->params = params;
    m->param_len = param_len;
    m->body = body;
}

static bool is_once_file(char *path) {
    for (OnceFile *f = once_files; f; f = f->next)
        if (strcmp(f->path, path) == 0)
            return true;
    return false;
}

static void mark_once_file(char *path) {
    if (is_once_file(path))
        return;
    OnceFile *f = arena_alloc(sizeof(OnceFile));
    f->path = path;
    f->next = once_files;
    once_files = f;
}

static char *resolve_include(char *curr_file, char *spec) {
    char *dir = path_dirname(curr_file);
    char *path = path_join(dir, spec);
    if (file_exists(path))
        return canonical_path(path);

    char *base = path_basename(spec);
    path = path_join(dir, base);
    if (file_exists(path))
        return canonical_path(path);

    path = path_join("include", spec);
    if (file_exists(path))
        return canonical_path(path);

    if (file_exists(spec))
        return canonical_path(spec);
    return NULL;
}

static char *expand_text(char *text, char *filename, int line_no, int depth);

static int find_param_index(Macro *m, char *name) {
    for (int i = 0; i < m->param_len; i++)
        if (strcmp(m->params[i], name) == 0)
            return i;
    return -1;
}

static char *trim_copy(char *p, int len) {
    while (len > 0 && isspace((unsigned char)*p)) {
        p++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        len--;
    return pp_strndup(p, len);
}

static char *quote_string(char *s) {
    StrBuf sb;
    sb_init(&sb, strlen(s) * 2 + 16);
    sb_putc(&sb, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            sb_putc(&sb, '\\');
        sb_putc(&sb, *s);
    }
    sb_putc(&sb, '"');
    return sb.buf;
}

static char *paste_tokens(char *s) {
    StrBuf sb;
    sb_init(&sb, strlen(s) + 16);
    for (char *p = s; *p;) {
        if (p[0] == '#' && p[1] == '#') {
            while (sb.len > 0 && isspace((unsigned char)sb.buf[sb.len - 1]))
                sb.buf[--sb.len] = '\0';
            p += 2;
            while (isspace((unsigned char)*p))
                p++;
            if (sb.len > 0 && *p && sb.buf[sb.len - 1] == *p &&
                strchr("+-&|<>", *p))
                sb_putc(&sb, ' ');
            continue;
        }
        sb_putc(&sb, *p++);
    }
    return sb.buf;
}

static char *substitute_macro(Macro *m, char **args, int argc, char *filename, int line_no, int depth) {
    StrBuf sb;
    sb_init(&sb, strlen(m->body) * 8 + 256);

    for (char *p = m->body; *p;) {
        if (p[0] == '#' && p[1] == '#') {
            sb_putc(&sb, '#');
            sb_putc(&sb, '#');
            p += 2;
            continue;
        }

        if (*p == '#' && p[1] != '#') {
            p++;
            while (isspace((unsigned char)*p))
                p++;
            if (pp_is_ident1(*p)) {
                char *start = p;
                while (pp_is_ident2(*p))
                    p++;
                char *name = pp_strndup(start, p - start);
                int idx = find_param_index(m, name);
                if (idx >= 0 && idx < argc) {
                    sb_puts(&sb, quote_string(args[idx]));
                    continue;
                }
                sb_putc(&sb, '#');
                sb_puts(&sb, name);
                continue;
            }
            sb_putc(&sb, '#');
            continue;
        }

        if (pp_is_ident1(*p)) {
            char *start = p;
            while (pp_is_ident2(*p))
                p++;
            char *name = pp_strndup(start, p - start);
            int idx = find_param_index(m, name);
            if (idx >= 0 && idx < argc) {
                sb_puts(&sb, expand_text(args[idx], filename, line_no, depth + 1));
                continue;
            }
            sb_puts(&sb, name);
            continue;
        }

        sb_putc(&sb, *p++);
    }

    return paste_tokens(sb.buf);
}

static int parse_macro_args(char *p, char **args, int max_args, char **end_out) {
    int argc = 0;
    int depth = 1;
    char *arg_start = p;

    while (*p) {
        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            while (*p && *p != quote) {
                if (*p == '\\' && p[1])
                    p += 2;
                else
                    p++;
            }
            if (*p)
                p++;
            continue;
        }

        if (*p == '(') {
            depth++;
            p++;
            continue;
        }
        if (*p == ')') {
            depth--;
            if (depth == 0) {
                args[argc++] = trim_copy(arg_start, p - arg_start);
                *end_out = p + 1;
                return argc == 1 && args[0][0] == '\0' ? 0 : argc;
            }
            p++;
            continue;
        }
        if (*p == ',' && depth == 1) {
            if (argc >= max_args)
                error("too many macro arguments");
            args[argc++] = trim_copy(arg_start, p - arg_start);
            arg_start = p + 1;
            p++;
            continue;
        }
        p++;
    }

    error("unterminated macro invocation");
    return 0;
}

static char *expand_text(char *text, char *filename, int line_no, int depth) {
    if (depth > 20)
        return text;

    StrBuf sb;
    sb_init(&sb, strlen(text) * 16 + 256);

    for (char *p = text; *p;) {
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            sb_putc(&sb, *p++);
            while (*p && *p != quote) {
                if (*p == '\\' && p[1]) {
                    sb_putc(&sb, *p++);
                    sb_putc(&sb, *p++);
                } else {
                    sb_putc(&sb, *p++);
                }
            }
            if (*p)
                sb_putc(&sb, *p++);
            continue;
        }

        if (!pp_is_ident1(*p)) {
            sb_putc(&sb, *p++);
            continue;
        }

        char *start = p;
        while (pp_is_ident2(*p))
            p++;
        char *name = pp_strndup(start, p - start);

        if (strcmp(name, "__LINE__") == 0) {
            sb_puts(&sb, format("%d", line_no));
            continue;
        }
        if (strcmp(name, "__FILE__") == 0) {
            sb_puts(&sb, quote_string(filename));
            continue;
        }
        if (strcmp(name, "__COUNTER__") == 0) {
            sb_puts(&sb, format("%d", pp_counter++));
            continue;
        }
        if (strcmp(name, "__FUNCTION__") == 0 || strcmp(name, "__func__") == 0) {
            sb_puts(&sb, quote_string("function"));
            continue;
        }
        if (strcmp(name, "__has_include") == 0 || strcmp(name, "__has_include_next") == 0) {
            char *q = p;
            while (isspace((unsigned char)*q))
                q++;
            if (*q == '(') {
                sb_puts(&sb, name);
                continue;
            }
        }

        Macro *m = find_macro(name);
        if (!m) {
            sb_puts(&sb, name);
            continue;
        }

        if (m->is_function) {
            char *q = p;
            while (isspace((unsigned char)*q))
                q++;
            if (*q != '(') {
                sb_puts(&sb, name);
                continue;
            }

            char *args[32];
            char *end = NULL;
            int argc = parse_macro_args(q + 1, args, 32, &end);
            char *subst = substitute_macro(m, args, argc, filename, line_no, depth + 1);
            sb_puts(&sb, expand_text(subst, filename, line_no, depth + 1));
            p = end;
            continue;
        }

        sb_puts(&sb, expand_text(m->body, filename, line_no, depth + 1));
    }

    if (strcmp(sb.buf, text) != 0)
        return expand_text(sb.buf, filename, line_no, depth + 1);
    return sb.buf;
}

static char *skip_spaces(char *p) {
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
}

static long eval_pp_expr(char **rest, char *p, char *filename);

static long eval_primary(char **rest, char *p, char *filename) {
    p = skip_spaces(p);

    if (pp_startswith(p, "defined")) {
        p += 7;
        p = skip_spaces(p);
        if (*p == '(') {
            p++;
            p = skip_spaces(p);
        }
        char *start = p;
        while (pp_is_ident2(*p))
            p++;
        char *name = pp_strndup(start, p - start);
        p = skip_spaces(p);
        if (*p == ')')
            p++;
        *rest = p;
        return find_macro(name) != NULL;
    }

    if (pp_startswith(p, "__has_include")) {
        bool is_next = pp_startswith(p, "__has_include_next");
        p += is_next ? 18 : 13;
        p = skip_spaces(p);
        p = skip_spaces(skip_spaces(p));
        if (*p == '(')
            p++;
        p = skip_spaces(p);
        char *spec = NULL;
        if (*p == '"') {
            char *start = ++p;
            while (*p && *p != '"')
                p++;
            spec = pp_strndup(start, p - start);
            if (*p == '"')
                p++;
        } else {
            char *start = p;
            while (*p && *p != ')')
                p++;
            spec = trim_copy(start, p - start);
            if (*spec == '"') {
                spec[strlen(spec) - 1] = '\0';
                spec++;
            } else {
                spec = expand_text(spec, filename, 1, 0);
                if (*spec == '"') {
                    spec[strlen(spec) - 1] = '\0';
                    spec++;
                }
            }
        }
        if (*p == ')')
            p++;
        *rest = p;
        return resolve_include(filename, spec) != NULL;
    }

    if (*p == '(') {
        long val = eval_pp_expr(&p, p + 1, filename);
        p = skip_spaces(p);
        if (*p == ')')
            p++;
        *rest = p;
        return val;
    }

    if (*p == '!') {
        long val = !eval_primary(&p, p + 1, filename);
        *rest = p;
        return val;
    }

    if (*p == '-') {
        long val = -eval_primary(&p, p + 1, filename);
        *rest = p;
        return val;
    }

    if (isdigit((unsigned char)*p)) {
        long val = strtol(p, &p, 0);
        *rest = p;
        return val;
    }

    if (pp_is_ident1(*p)) {
        char *start = p;
        while (pp_is_ident2(*p))
            p++;
        char *name = pp_strndup(start, p - start);
        Macro *m = find_macro(name);
        *rest = p;
        if (!m || m->is_function)
            return 0;
        return strtol(expand_text(m->body, filename, 1, 0), NULL, 0);
    }

    *rest = p;
    return 0;
}

static long eval_mul(char **rest, char *p, char *filename) {
    long val = eval_primary(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (*p == '*') val *= eval_primary(&p, p + 1, filename);
        else if (*p == '/') { long rhs = eval_primary(&p, p + 1, filename); val = rhs ? val / rhs : 0; }
        else if (*p == '%') { long rhs = eval_primary(&p, p + 1, filename); val = rhs ? val % rhs : 0; }
        else break;
    }
    *rest = p;
    return val;
}

static long eval_add(char **rest, char *p, char *filename) {
    long val = eval_mul(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (*p == '+') val += eval_mul(&p, p + 1, filename);
        else if (*p == '-') val -= eval_mul(&p, p + 1, filename);
        else break;
    }
    *rest = p;
    return val;
}

static long eval_rel(char **rest, char *p, char *filename) {
    long val = eval_add(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (pp_startswith(p, "<=")) val = val <= eval_add(&p, p + 2, filename);
        else if (pp_startswith(p, ">=")) val = val >= eval_add(&p, p + 2, filename);
        else if (*p == '<') val = val < eval_add(&p, p + 1, filename);
        else if (*p == '>') val = val > eval_add(&p, p + 1, filename);
        else break;
    }
    *rest = p;
    return val;
}

static long eval_eq(char **rest, char *p, char *filename) {
    long val = eval_rel(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (pp_startswith(p, "==")) val = val == eval_rel(&p, p + 2, filename);
        else if (pp_startswith(p, "!=")) val = val != eval_rel(&p, p + 2, filename);
        else break;
    }
    *rest = p;
    return val;
}

static long eval_land(char **rest, char *p, char *filename) {
    long val = eval_eq(&p, p, filename);
    while (pp_startswith(skip_spaces(p), "&&")) {
        p = skip_spaces(p);
        long rhs = eval_eq(&p, p + 2, filename);
        val = val && rhs;
    }
    *rest = p;
    return val;
}

static long eval_pp_expr(char **rest, char *p, char *filename) {
    long val = eval_land(&p, p, filename);
    while (pp_startswith(skip_spaces(p), "||")) {
        p = skip_spaces(p);
        long rhs = eval_land(&p, p + 2, filename);
        val = val || rhs;
    }
    *rest = p;
    return val;
}

static long eval_condition(char *expr, char *filename) {
    char *rest = expr;
    return eval_pp_expr(&rest, expr, filename);
}

static char *preprocess_file(char *filename, char *input) {
    if (is_once_file(filename))
        return "";

    StrBuf out;
    sb_init(&out, strlen(input) * 16 + 65536);
    CondIncl *conds = NULL;
    bool active = true;
    int line_no = 1;

    for (char *p = input; *p;) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        char *end = p;
        if (*p == '\n')
            p++;

        char *s = line;
        while (s < end && (*s == ' ' || *s == '\t'))
            s++;

        if (s < end && *s == '#') {
            s++;
            while (s < end && isspace((unsigned char)*s))
                s++;

            if (pp_startswith(s, "pragma")) {
                s += 6;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                if (pp_startswith(s, "once"))
                    mark_once_file(filename);
            } else if (pp_startswith(s, "define") && active) {
                s += 6;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                bool is_function = false;
                char **params = NULL;
                int param_len = 0;
                if (s < end && *s == '(') {
                    is_function = true;
                    params = arena_alloc(sizeof(char *) * 32);
                    s++;
                    while (s < end && *s != ')') {
                        while (s < end && isspace((unsigned char)*s))
                            s++;
                        char *ps = s;
                        while (s < end && pp_is_ident2(*s))
                            s++;
                        if (s > ps)
                            params[param_len++] = pp_strndup(ps, s - ps);
                        while (s < end && isspace((unsigned char)*s))
                            s++;
                        if (*s == ',')
                            s++;
                    }
                    if (s < end && *s == ')')
                        s++;
                }
                while (s < end && (*s == ' ' || *s == '\t'))
                    s++;
                define_macro(name, is_function, params, param_len, trim_copy(s, end - s));
            } else if (pp_startswith(s, "undef") && active) {
                s += 5;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                Macro **pm = &macros;
                while (*pm) {
                    if (strcmp((*pm)->name, name) == 0) {
                        *pm = (*pm)->next;
                        break;
                    }
                    pm = &(*pm)->next;
                }
            } else if (pp_startswith(s, "include") && active) {
                s += 7;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                if (s < end && (*s == '"' || *s == '<')) {
                    char close = (*s == '"') ? '"' : '>';
                    char *start = ++s;
                    while (s < end && *s != close)
                        s++;
                    char *spec = pp_strndup(start, s - start);
                    char *path = resolve_include(filename, spec);
                    if (path) {
                        char *inc = read_pp_file(path);
                        if (inc)
                            sb_puts(&out, preprocess_file(path, inc));
                    }
                }
            } else if (pp_startswith(s, "ifdef")) {
                s += 5;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                CondIncl *ci = arena_alloc(sizeof(CondIncl));
                ci->parent_active = active;
                ci->active = active && find_macro(name);
                ci->branch_taken = ci->active;
                ci->next = conds;
                conds = ci;
                active = ci->active;
            } else if (pp_startswith(s, "ifndef")) {
                s += 6;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                CondIncl *ci = arena_alloc(sizeof(CondIncl));
                ci->parent_active = active;
                ci->active = active && !find_macro(name);
                ci->branch_taken = ci->active;
                ci->next = conds;
                conds = ci;
                active = ci->active;
            } else if (pp_startswith(s, "if")) {
                s += 2;
                CondIncl *ci = arena_alloc(sizeof(CondIncl));
                ci->parent_active = active;
                ci->active = active && eval_condition(trim_copy(s, end - s), filename);
                ci->branch_taken = ci->active;
                ci->next = conds;
                conds = ci;
                active = ci->active;
            } else if (pp_startswith(s, "elif")) {
                s += 4;
                if (conds) {
                    if (!conds->parent_active || conds->branch_taken)
                        conds->active = false;
                    else
                        conds->active = eval_condition(trim_copy(s, end - s), filename);
                    if (conds->active)
                        conds->branch_taken = true;
                    active = conds->active;
                }
            } else if (pp_startswith(s, "else")) {
                if (conds) {
                    conds->active = conds->parent_active && !conds->branch_taken;
                    conds->branch_taken = true;
                    active = conds->active;
                }
            } else if (pp_startswith(s, "endif")) {
                if (conds) {
                    CondIncl *next = conds->next;
                    active = conds->parent_active;
                    conds = next;
                }
            }
        } else if (active) {
            char *expanded = expand_text(pp_strndup(line, end - line), filename, line_no, 0);
            sb_puts(&out, expanded);
            sb_putc(&out, '\n');
        } else {
            sb_putc(&out, '\n');
        }

        line_no++;
    }

    return out.buf;
}

char *preprocess(char *filename, char *p) {
    static char *builtin_expect_params[] = {"x", "y"};

    if (!find_macro("__has_include"))
        define_macro("__has_include", false, NULL, 0, "1");
    if (!find_macro("__has_include_next"))
        define_macro("__has_include_next", false, NULL, 0, "1");
    if (!find_macro("__SIZE_TYPE__"))
        define_macro("__SIZE_TYPE__", false, NULL, 0, "unsigned long long");
    if (!find_macro("__WCHAR_TYPE__"))
        define_macro("__WCHAR_TYPE__", false, NULL, 0, "unsigned short");
    if (!find_macro("_Atomic"))
        define_macro("_Atomic", false, NULL, 0, "");
    if (!find_macro("_WIN32"))
        define_macro("_WIN32", false, NULL, 0, "1");
    if (!find_macro("__x86_64__"))
        define_macro("__x86_64__", false, NULL, 0, "1");
    if (!find_macro("__LLP64__"))
        define_macro("__LLP64__", false, NULL, 0, "1");
    if (!find_macro("__builtin_expect"))
        define_macro("__builtin_expect", true, builtin_expect_params, 2, "x");
    if (!find_macro("__builtin_abort"))
        define_macro("__builtin_abort", false, NULL, 0, "abort");
    if (!find_macro("__builtin_malloc"))
        define_macro("__builtin_malloc", false, NULL, 0, "malloc");
    if (!find_macro("__builtin_calloc"))
        define_macro("__builtin_calloc", false, NULL, 0, "calloc");
    if (!find_macro("__builtin_realloc"))
        define_macro("__builtin_realloc", false, NULL, 0, "realloc");
    if (!find_macro("__builtin_free"))
        define_macro("__builtin_free", false, NULL, 0, "free");
    if (!find_macro("__builtin_memcpy"))
        define_macro("__builtin_memcpy", false, NULL, 0, "memcpy");
    if (!find_macro("__builtin_memcmp"))
        define_macro("__builtin_memcmp", false, NULL, 0, "memcmp");
    if (!find_macro("__builtin_memmove"))
        define_macro("__builtin_memmove", false, NULL, 0, "memmove");
    if (!find_macro("__builtin_memset"))
        define_macro("__builtin_memset", false, NULL, 0, "memset");
    if (!find_macro("__builtin_strlen"))
        define_macro("__builtin_strlen", false, NULL, 0, "strlen");
    if (!find_macro("__builtin_strcpy"))
        define_macro("__builtin_strcpy", false, NULL, 0, "strcpy");
    if (!find_macro("__builtin_strncpy"))
        define_macro("__builtin_strncpy", false, NULL, 0, "strncpy");
    if (!find_macro("__builtin_strcmp"))
        define_macro("__builtin_strcmp", false, NULL, 0, "strcmp");
    if (!find_macro("__builtin_strncmp"))
        define_macro("__builtin_strncmp", false, NULL, 0, "strncmp");
    if (!find_macro("__builtin_strcat"))
        define_macro("__builtin_strcat", false, NULL, 0, "strcat");
    if (!find_macro("__builtin_strncat"))
        define_macro("__builtin_strncat", false, NULL, 0, "strncat");
    if (!find_macro("__builtin_strchr"))
        define_macro("__builtin_strchr", false, NULL, 0, "strchr");
    if (!find_macro("__builtin_strrchr"))
        define_macro("__builtin_strrchr", false, NULL, 0, "strrchr");
    if (!find_macro("__builtin_strdup"))
        define_macro("__builtin_strdup", false, NULL, 0, "strdup");
    if (!find_macro("__builtin_alloca"))
        define_macro("__builtin_alloca", false, NULL, 0, "alloca");
    if (!find_macro("__GNUC__"))
        define_macro("__GNUC__", false, NULL, 0, "4");
    if (!find_macro("__GNUC_MINOR__"))
        define_macro("__GNUC_MINOR__", false, NULL, 0, "0");
    if (!find_macro("__STDC__"))
        define_macro("__STDC__", false, NULL, 0, "1");
    if (!find_macro("__STDC_VERSION__"))
        define_macro("__STDC_VERSION__", false, NULL, 0, "201112L");
    if (!find_macro("__extension__"))
        define_macro("__extension__", false, NULL, 0, "");
    if (!find_macro("__asm__"))
        define_macro("__asm__", false, NULL, 0, "__asm");
    if (!find_macro("__volatile__"))
        define_macro("__volatile__", false, NULL, 0, "volatile");
    if (!find_macro("__INT_MAX__"))
        define_macro("__INT_MAX__", false, NULL, 0, "2147483647");
    if (!find_macro("__LONG_MAX__"))
        define_macro("__LONG_MAX__", false, NULL, 0, "2147483647L");
    if (!find_macro("__LONG_LONG_MAX__"))
        define_macro("__LONG_LONG_MAX__", false, NULL, 0, "9223372036854775807LL");
    if (!find_macro("__SIZEOF_INT__"))
        define_macro("__SIZEOF_INT__", false, NULL, 0, "4");
    if (!find_macro("__SIZEOF_LONG__"))
        define_macro("__SIZEOF_LONG__", false, NULL, 0, "4");
    if (!find_macro("__SIZEOF_LONG_LONG__"))
        define_macro("__SIZEOF_LONG_LONG__", false, NULL, 0, "8");
    if (!find_macro("__SIZEOF_POINTER__"))
        define_macro("__SIZEOF_POINTER__", false, NULL, 0, "8");
    if (!find_macro("__SIZEOF_FLOAT__"))
        define_macro("__SIZEOF_FLOAT__", false, NULL, 0, "4");
    if (!find_macro("__SIZEOF_DOUBLE__"))
        define_macro("__SIZEOF_DOUBLE__", false, NULL, 0, "8");
    return preprocess_file(canonical_path(filename), splice_lines(p));
}
