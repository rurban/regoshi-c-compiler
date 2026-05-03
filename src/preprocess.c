// SPDX-License-Identifier: LGPL-2.1-or-later
// Parts derived from chibicc by Rui Ueyama.
#include "rcc.h"
#include <ctype.h>
#include <stdlib.h>

#ifdef _WIN32
#define PATHSEP "\\"
#else
#define PATHSEP "/"
#endif

typedef struct Macro Macro;
typedef struct OnceFile OnceFile;
typedef struct CondIncl CondIncl;

struct Macro {
    Macro *next;
    Macro *hash_next; // hash-table chain
    uint32_t hash; // cached hash value
    char *name;
    bool is_function;
    bool is_variadic;
    char **params;
    int param_len;
    char *body;
};

#define MACRO_HT_SIZE 2048 // power of 2
static Macro *macro_htab[MACRO_HT_SIZE];

static uint32_t macro_hash(const char *name) {
    uint32_t h = 2166136261u;
    for (; *name; name++) {
        h ^= (uint8_t)*name;
        h *= 16777619;
    }
    return h & (MACRO_HT_SIZE - 1);
}

static void macro_ht_add(Macro *m) {
    uint32_t h = macro_hash(m->name);
    m->hash = h;
    m->hash_next = macro_htab[h];
    macro_htab[h] = m;
}

static void macro_ht_remove(const char *name) {
    uint32_t h = macro_hash(name);
    for (Macro **p = &macro_htab[h]; *p; p = &(*p)->hash_next) {
        if (strcmp((*p)->name, name) == 0) {
            *p = (*p)->hash_next;
            return;
        }
    }
}

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

typedef struct MacroStack MacroStack;
struct MacroStack {
    MacroStack *next;
    char *name;
    bool is_function;
    bool is_variadic;
    char **params;
    int param_len;
    char *body;
};

typedef struct {
    char *buf;
    int len;
    int cap;
} StrBuf;

static Macro *macros;
static OnceFile *once_files;
static int pp_counter;
static Macro *cmdline_macros;
static Macro *saved_macros; // cmdline + builtins snapshot for fast reset
static MacroStack *macro_stack;

static const char *user_include_paths[64];
static int nb_user_include_paths;

void add_include_path(const char *path) {
    if (nb_user_include_paths < 64)
        user_include_paths[nb_user_include_paths++] = str_intern(path, strlen(path));
}

static void clear_macros(void) {
    macros = saved_macros ? saved_macros : cmdline_macros;
    macro_stack = NULL;
    memset(macro_htab, 0, sizeof(macro_htab));
    for (Macro *m = macros; m; m = m->next)
        macro_ht_add(m);
}

int pack_align = 0;
int pack_align_stack[16];
int pack_align_idx = 0;

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

static void sb_puts(StrBuf *sb, char *s);

static char *dump_macros(void) {
    StrBuf sb;
    sb_init(&sb, 4096);
    for (Macro *m = macros; m; m = m->next) {
        sb_puts(&sb, "#define ");
        sb_puts(&sb, m->name);
        if (m->is_function) {
            sb_putc(&sb, '(');
            for (int i = 0; i < m->param_len; i++) {
                if (i > 0)
                    sb_puts(&sb, ", ");
                sb_puts(&sb, m->params[i]);
            }
            if (m->is_variadic) {
                if (m->param_len > 0)
                    sb_puts(&sb, ", ");
                sb_puts(&sb, "...");
            }
            sb_putc(&sb, ')');
        }
        sb_putc(&sb, ' ');
        sb_puts(&sb, m->body);
        sb_putc(&sb, '\n');
    }
    return sb.buf;
}

static void sb_puts(StrBuf *sb, char *s) {
    int len = strlen(s);
    sb_reserve(sb, sb->len + len + 1);
    memcpy(sb->buf + sb->len, s, len);
    sb->len += len;
    sb->buf[sb->len] = '\0';
}

typedef struct {
    char *text;
    int *line_counts;
} SplicedInput;

static SplicedInput splice_lines_with_counts(char *input) {
    int len = strlen(input);
    char *buf = arena_alloc(len + 1);
    int *counts = arena_alloc(sizeof(int) * (len + 1));
    int j = 0;
    int line_idx = 0;
    int count = 1;

    for (int i = 0; i < len; i++) {
        if (input[i] == '\\' && input[i + 1] == '\n') {
            i++;
            count++;
        } else {
            buf[j++] = input[i];
            if (input[i] == '\n') {
                counts[line_idx++] = count;
                count = 1;
            }
        }
    }
    if (count > 1 || j == 0 || buf[j - 1] != '\n') {
        counts[line_idx++] = count;
    }
    buf[j] = '\0';

    SplicedInput result;
    result.text = buf;
    result.line_counts = counts;
    return result;
}

static char *path_dirname(char *path) {
    char *last = path;
    for (char *p = path; *p; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            last = p + 1;
    }
    return pp_strndup(path, last - path);
}

char *path_basename(char *path) {
    char *last = path;
    for (char *p = path; *p; p++) {
#ifdef _WIN32
        if (*p == '/' || *p == '\\')
#else
        if (*p == '/')
#endif
            last = p + 1;
    }
    return last;
}

static char *path_join(const char *dir, const char *file) {
    if (!*dir)
        return str_intern(file, strlen(file));
#ifdef _WIN32
    return format("%s%s%s", dir, (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') ? "" : PATHSEP, file);
#else
    return format("%s%s%s", dir, (dir[strlen(dir) - 1] == '/') ? "" : PATHSEP, file);
#endif
}

static bool file_exists(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        return true;
    }
    return false;
}

static char *full_path(char *path) {
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

static char *canonical_path(char *path) {
    if (!path || !*path)
        return str_intern(path, strlen(path));

    char buf[4096];
    int len = strlen(path);
    if (len >= (int)sizeof(buf))
        return str_intern(path, strlen(path));
    memcpy(buf, path, len + 1);

#ifdef _WIN32
    for (int i = 0; i < len; i++)
        if (buf[i] == '\\')
            buf[i] = '/';
#endif

    char *comps[256];
    int comp_lens[256];
    int ncomp = 0;

    char *p = buf;
#ifdef _WIN32
    bool absolute = false;
    if (len >= 2 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        absolute = true;
        p += 2;
    } else if (*p == '/') {
        absolute = true;
        p++;
    }
    while (*p == '/')
        p++;
#else
    bool absolute = (*p == '/');
    if (absolute)
        p++;
#endif

    while (*p) {
        char *start = p;
        while (*p && *p != '/')
            p++;
        int clen = p - start;
        while (*p == '/')
            p++;

        if (clen == 1 && start[0] == '.') {
            continue;
        } else if (clen == 2 && start[0] == '.' && start[1] == '.') {
            if (ncomp > 0 && !(comp_lens[ncomp - 1] == 2 && comps[ncomp - 1][0] == '.' && comps[ncomp - 1][1] == '.')) {
                ncomp--;
            } else if (!absolute) {
                comps[ncomp] = start;
                comp_lens[ncomp] = clen;
                ncomp++;
            }
        } else {
            comps[ncomp] = start;
            comp_lens[ncomp] = clen;
            ncomp++;
        }
    }

    char out[4096];
    int dst = 0;
    if (absolute) {
#ifdef _WIN32
        if (buf[0] == '/') {
            out[dst++] = '/';
        } else {
            out[dst++] = buf[0];
            out[dst++] = ':';
            out[dst++] = '/';
        }
#else
        out[dst++] = '/';
#endif
    }

    for (int i = 0; i < ncomp; i++) {
        if (i > 0)
            out[dst++] = '/';
        memcpy(out + dst, comps[i], comp_lens[i]);
        dst += comp_lens[i];
    }

    if (dst == 0)
        out[dst++] = '.';
    out[dst] = '\0';

    return str_intern(out, dst);
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

// Replace C/C++ comments with spaces. Preserves newlines so line numbers
// stay intact. Must be called before preprocess_file() so that comments in
// #define bodies (e.g. #define P //_P) are stripped before the macro is stored.
static void strip_comments(char *p) {
    bool in_string = false;
    while (*p) {
        if (in_string) {
            if (*p == '\n') {
                in_string = false;
                p++;
            } else if (*p == '\\' && p[1]) {
                p += 2;
            } else if (*p == '"') {
                in_string = false;
                p++;
            } else {
                p++;
            }
        } else {
            if (*p == '"') {
                in_string = true;
                p++;
            } else if (p[0] == '/' && p[1] == '/') {
                while (*p && *p != '\n')
                    *p++ = ' ';
            } else if (p[0] == '/' && p[1] == '*') {
                p[0] = ' ';
                p[1] = ' ';
                p += 2;
                while (*p && !(p[0] == '*' && p[1] == '/')) {
                    if (*p != '\n')
                        *p = ' ';
                    p++;
                }
                if (*p) {
                    *p = ' ';
                    p++;
                }
                if (*p) {
                    *p = ' ';
                    p++;
                }
            } else {
                p++;
            }
        }
    }
}

static Macro *find_macro(char *name) {
    uint32_t h = macro_hash(name);
    for (Macro *m = macro_htab[h]; m; m = m->hash_next)
        if (m->hash == h && strcmp(m->name, name) == 0)
            return m;
    return NULL;
}

static void push_macro(char *name) {
    MacroStack *ms = arena_alloc(sizeof(MacroStack));
    ms->name = name;
    Macro *m = find_macro(name);
    if (m) {
        ms->is_function = m->is_function;
        ms->is_variadic = m->is_variadic;
        ms->param_len = m->param_len;
        ms->params = m->params;
        ms->body = m->body;
    } else {
        ms->is_function = false;
        ms->param_len = -1;
        ms->params = NULL;
        ms->body = NULL;
    }
    ms->next = macro_stack;
    macro_stack = ms;
}

static void pop_macro(char *name) {
    MacroStack *ms = NULL;
    for (MacroStack **p = &macro_stack; *p; p = &(*p)->next) {
        if (strcmp((*p)->name, name) == 0) {
            ms = *p;
            *p = (*p)->next;
            break;
        }
    }
    if (!ms) return;
    if (ms->param_len < 0) {
        // Remove the macro from both linked list and hash table
        macro_ht_remove(name);
        Macro **pm = &macros;
        while (*pm) {
            if (strcmp((*pm)->name, name) == 0) {
                *pm = (*pm)->next;
                break;
            }
            pm = &(*pm)->next;
        }
    } else {
        Macro *m = find_macro(name);
        if (!m) {
            m = arena_alloc(sizeof(Macro));
            m->name = name;
            m->next = macros;
            macros = m;
            macro_ht_add(m);
        }
        m->is_function = ms->is_function;
        m->is_variadic = ms->is_variadic;
        m->param_len = ms->param_len;
        m->params = ms->params;
        m->body = ms->body;
    }
}

static void define_macro(char *name, bool is_function, char **params, int param_len, char *body) {
    Macro *m = find_macro(name);
    if (!m) {
        m = arena_alloc(sizeof(Macro));
        m->name = name; // must be set before macro_ht_add
        m->next = macros;
        macros = m;
        macro_ht_add(m);
    }
    m->name = name;
    m->is_function = is_function;
    m->is_variadic = false;
    m->params = params;
    m->param_len = param_len;
    m->body = body;
}

void add_define(char *def) {
    char *eq = strchr(def, '=');
    char *name;
    char *body;
    if (eq) {
        name = pp_strndup(def, eq - def);
        body = strdup(eq + 1);
    } else {
        name = strdup(def);
        body = strdup("1");
    }
    Macro *m = arena_alloc(sizeof(Macro));
    m->name = name;
    m->is_function = false;
    m->params = NULL;
    m->param_len = 0;
    m->body = body;
    m->next = cmdline_macros;
    cmdline_macros = m;
}

void add_undef(char *name) {
    macro_ht_remove(name);
    Macro **prev = &macros;
    for (Macro *m = macros; m; prev = &m->next, m = m->next) {
        if (strcmp(m->name, name) == 0) {
            *prev = m->next;
            return;
        }
    }
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

#include "sysinc_paths.h"

static char *resolve_include(char *curr_file, char *spec) {
    char *dir = path_dirname(curr_file);
    char *path = path_join(dir, spec);
    //fprintf(stderr, "resolve_include: %s (curr_file=%s, spec=%s))\n", path, curr_file, spec);
    if (file_exists(path))
        return canonical_path(path);

#ifndef RCC_INCDIR
#define RCC_INCDIR "include"
#endif
    path = path_join(RCC_INCDIR, spec);
    //fprintf(stderr, "resolve_include: %s (RCC_INCDIR=%s)\n", path, RCC_INCDIR);
    if (file_exists(path))
        return canonical_path(path);
    if (strcmp(RCC_INCDIR, "include") != 0) {
        path = path_join("include", spec);
        //fprintf(stderr, "resolve_include: %s (RCC_INCDIR=%s)\n", path, RCC_INCDIR);
        if (file_exists(path))
            return canonical_path(path);
    }

    for (int i = 0; i < nb_user_include_paths; i++) {
        path = path_join(user_include_paths[i], spec);
        //fprintf(stderr, "resolve_include: %s\n", path);
        if (file_exists(path))
            return canonical_path(path);
    }

    for (int i = 0; sys_include_paths[i]; i++) {
        path = path_join(sys_include_paths[i], spec);
        //fprintf(stderr, "resolve_include: %s\n", path);
        if (file_exists(path))
            return canonical_path(path);
    }

    //fprintf(stderr, "resolve_include: %s\n", spec);
    if (file_exists(spec))
        return canonical_path(spec);
    //fprintf(stderr, "resolve_include: spec=%s not found\n", spec);
    return NULL;
}

static char *expand_text(char *text, char *filename, unsigned line_no, int depth);

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

    // For variadic macros, merge excess args into the last variadic slot
    if (m->is_variadic && argc > m->param_len) {
        // Merge args[m->param_len] through args[argc-1] with commas
        StrBuf va_buf;
        sb_init(&va_buf, 256);
        for (int i = m->param_len; i < argc; i++) {
            if (i > m->param_len) sb_putc(&va_buf, ',');
            sb_puts(&va_buf, args[i]);
        }
        // Replace the slot with merged args
        args[m->param_len] = va_buf.buf;
        argc = m->param_len + 1;
    }

    for (char *p = m->body; *p;) {
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
            if (*p) sb_putc(&sb, *p++);
            continue;
        }
        if (p[0] == '#' && p[1] == '#') {
            p += 2;
            while (isspace((unsigned char)*p))
                p++;
            // GNU extension: ,##__VA_ARGS__ — delete comma if __VA_ARGS__ is empty
            if (m->is_variadic && pp_startswith(p, "__VA_ARGS__") && !pp_is_ident2(p[11])) {
                if (m->param_len >= argc) {
                    // Strip trailing comma and whitespace from output
                    while (sb.len > 0 && isspace((unsigned char)sb.buf[sb.len - 1]))
                        sb.buf[--sb.len] = '\0';
                    if (sb.len > 0 && sb.buf[sb.len - 1] == ',')
                        sb.buf[--sb.len] = '\0';
                } else {
                    sb_puts(&sb, args[m->param_len]);
                }
                p += 11;
                continue;
            }
            sb_putc(&sb, '#');
            sb_putc(&sb, '#');
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
            // Handle __VA_ARGS__ in variadic macros
            if (m->is_variadic && strcmp(name, "__VA_ARGS__") == 0) {
                if (m->param_len < argc) {
                    sb_puts(&sb, args[m->param_len]);
                }
                continue;
            }
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

static char *expand_text(char *text, char *filename, unsigned line_no, int depth) {
    if (depth > 20)
        return text;

    StrBuf sb;
    sb_init(&sb, strlen(text) + 256);

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
            sb_puts(&sb, format("%u", line_no));
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
        if (strcmp(name, "__FUNCTION__") == 0 || strcmp(name, "__func__") == 0 ||
            strcmp(name, "__PRETTY_FUNCTION__") == 0) {
            // Leave these as identifiers; the parser resolves them to the function name
            sb_puts(&sb, name);
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
        else if (*p == '/') {
            long rhs = eval_primary(&p, p + 1, filename);
            val = rhs ? val / rhs : 0;
        } else if (*p == '%') {
            long rhs = eval_primary(&p, p + 1, filename);
            val = rhs ? val % rhs : 0;
        } else
            break;
    }
    *rest = p;
    return val;
}

static long eval_add(char **rest, char *p, char *filename) {
    long val = eval_mul(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (*p == '+') val += eval_mul(&p, p + 1, filename);
        else if (*p == '-')
            val -= eval_mul(&p, p + 1, filename);
        else
            break;
    }
    *rest = p;
    return val;
}

static long eval_rel(char **rest, char *p, char *filename) {
    long val = eval_add(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (pp_startswith(p, "<=")) val = val <= eval_add(&p, p + 2, filename);
        else if (pp_startswith(p, ">="))
            val = val >= eval_add(&p, p + 2, filename);
        else if (*p == '<')
            val = val < eval_add(&p, p + 1, filename);
        else if (*p == '>')
            val = val > eval_add(&p, p + 1, filename);
        else
            break;
    }
    *rest = p;
    return val;
}

static long eval_eq(char **rest, char *p, char *filename) {
    long val = eval_rel(&p, p, filename);
    for (;;) {
        p = skip_spaces(p);
        if (pp_startswith(p, "==")) val = val == eval_rel(&p, p + 2, filename);
        else if (pp_startswith(p, "!="))
            val = val != eval_rel(&p, p + 2, filename);
        else
            break;
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
    p = skip_spaces(p);
    if (*p == '?') {
        p++;
        long true_val = eval_pp_expr(&p, p, filename);
        p = skip_spaces(p);
        if (*p == ':')
            p++;
        long false_val = eval_pp_expr(&p, p, filename);
        val = val ? true_val : false_val;
    }
    *rest = p;
    return val;
}

static long eval_condition(char *expr, char *filename) {
    char *rest = expr;
    return eval_pp_expr(&rest, expr, filename);
}

static int count_unmatched_parens(char *start, char *end) {
    int open = 0;
    int close = 0;

    while (start < end) {
        if (*start == '"' || *start == '\'') {
            char quote = *start++;
            while (start < end && *start != quote) {
                if (*start == '\\' && start + 1 < end)
                    start += 2;
                else
                    start++;
            }
            if (start < end)
                start++;
            continue;
        }
        if (*start == '(')
            open++;
        else if (*start == ')')
            close++;
        start++;
    }

    return open - close;
}

static char *preprocess_file(char *filename, char *input, int *line_counts) {
    char *fpath = full_path(filename);
    if (is_once_file(fpath))
        return "";

    strip_comments(input);

    StrBuf out;
    sb_init(&out, strlen(input) * 16 + 65536);
    CondIncl *conds = NULL;
    bool active = true;
    unsigned line_no = 1;
    int line_idx = 0;

    StrBuf acc;
    sb_init(&acc, 4096);
    unsigned acc_line_no = 0;
    int acc_line_count = 0;
    int acc_phys_count = 0;

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
            if (acc.len > 0) {
                char *expanded = expand_text(acc.buf, filename, acc_line_no, 0);
                sb_puts(&out, expanded);
                for (int i = 0; i < acc_phys_count; i++)
                    sb_putc(&out, '\n');
                acc.len = 0;
                acc.buf[0] = '\0';
                acc_line_count = 0;
                acc_phys_count = 0;
            }
            s++;
            while (s < end && isspace((unsigned char)*s))
                s++;

            if (pp_startswith(s, "pragma") && active) {
                s += 6;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                if (pp_startswith(s, "once")) {
                    mark_once_file(fpath);
                    sb_putc(&out, '\n');
                } else if (pp_startswith(s, "pack")) {
                    s += 4;
                    while (s < end && isspace((unsigned char)*s)) s++;
                    if (*s == '(') {
                        s++;
                        while (s < end && isspace((unsigned char)*s)) s++;
                    }
                    if (pp_startswith(s, "push")) {
                        pack_align_stack[pack_align_idx++] = pack_align;
                        s += 4;
                        while (s < end && isspace((unsigned char)*s)) s++;
                        if (*s == ',') {
                            s++;
                            while (s < end && isspace((unsigned char)*s)) s++;
                        }
                        if (*s >= '1' && *s <= '9')
                            pack_align = *s - '0';
                        // Emit so parser sees current pack state
                        // fprintf(stderr, "DEBUG PP: push pack_align=%d\n", pack_align);
                        sb_puts(&out, format("# pragma pack(%d)\n", pack_align));
                    } else if (pp_startswith(s, "pop")) {
                        pack_align = pack_align_stack[--pack_align_idx];
                        // fprintf(stderr, "DEBUG PP: pop pack_align=%d\n", pack_align);
                        sb_puts(&out, "# pragma pack()\n");
                    } else if (*s >= '1' && *s <= '9') {
                        pack_align = *s - '0';
                        // fprintf(stderr, "DEBUG PP: set pack_align=%d\n", pack_align);
                        sb_puts(&out, format("# pragma pack(%d)\n", pack_align));
                    }
                } else if (pp_startswith(s, "push_macro")) {
                    s += 10;
                    while (s < end && isspace((unsigned char)*s)) s++;
                    if (*s == '(') {
                        s++;
                        while (s < end && isspace((unsigned char)*s)) s++;
                        char *name_start = s;
                        if (*s == '"') {
                            name_start = ++s;
                            while (s < end && *s != '"') s++;
                        } else {
                            while (s < end && pp_is_ident2(*s)) s++;
                        }
                        char *name = pp_strndup(name_start, s - name_start);
                        push_macro(name);
                        sb_putc(&out, '\n');
                    }
                } else if (pp_startswith(s, "pop_macro")) {
                    s += 9;
                    while (s < end && isspace((unsigned char)*s)) s++;
                    if (*s == '(') {
                        s++;
                        while (s < end && isspace((unsigned char)*s)) s++;
                        char *name_start = s;
                        if (*s == '"') {
                            name_start = ++s;
                            while (s < end && *s != '"') s++;
                        } else {
                            while (s < end && pp_is_ident2(*s)) s++;
                        }
                        char *name = pp_strndup(name_start, s - name_start);
                        pop_macro(name);
                        sb_putc(&out, '\n');
                    }
                } else {
                    sb_putc(&out, '\n');
                }
            } else if (pp_startswith(s, "define") && active) {
                s += 6;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                bool is_function = false;
                bool is_variadic = false;
                char **params = NULL;
                int param_len = 0;
                if (s < end && *s == '(') {
                    is_function = true;
                    params = arena_alloc(sizeof(char *) * 32);
                    s++;
                    while (s < end && *s != ')') {
                        while (s < end && isspace((unsigned char)*s))
                            s++;
                        // Check for ... (variadic macro)
                        if (s[0] == '.' && s[1] == '.' && s[2] == '.') {
                            s += 3;
                            while (s < end && isspace((unsigned char)*s))
                                s++;
                            is_variadic = true;
                            if (s < end && *s == ')')
                                break;
                            if (*s == ',') {
                                s++;
                                continue;
                            }
                            break;
                        }
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
                    define_macro(name, is_function, params, param_len, trim_copy(s, end - s));
                    if (is_variadic) {
                        Macro *m = find_macro(name);
                        if (m) m->is_variadic = true;
                    }
                    {
                        int n = line_counts ? line_counts[line_idx] : 1;
                        if (line_counts)
                            line_no += line_counts[line_idx++];
                        else
                            line_no++;
                        for (int i = 0; i < n; i++)
                            sb_putc(&out, '\n');
                    }
                    continue;
                }
                while (s < end && (*s == ' ' || *s == '\t'))
                    s++;
                define_macro(name, is_function, params, param_len, trim_copy(s, end - s));
                {
                    int n = line_counts ? line_counts[line_idx] : 1;
                    for (int i = 0; i < n; i++)
                        sb_putc(&out, '\n');
                }
            } else if (pp_startswith(s, "undef") && active) {
                s += 5;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *name_start = s;
                while (s < end && pp_is_ident2(*s))
                    s++;
                char *name = pp_strndup(name_start, s - name_start);
                macro_ht_remove(name);
                Macro **pm = &macros;
                while (*pm) {
                    if (strcmp((*pm)->name, name) == 0) {
                        *pm = (*pm)->next;
                        break;
                    }
                    pm = &(*pm)->next;
                }
                {
                    int n = line_counts ? line_counts[line_idx] : 1;
                    for (int i = 0; i < n; i++)
                        sb_putc(&out, '\n');
                }
            } else if (pp_startswith(s, "include") && active) {
                s += 7;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *inc_arg = s;
                while (s < end && *s != '\n')
                    s++;
                char *inc_end = s;
                char *spec = NULL;
                char *expanded = NULL;
                if (inc_arg < inc_end && (*inc_arg == '"' || *inc_arg == '<')) {
                    char close = (*inc_arg == '"') ? '"' : '>';
                    char *start = ++inc_arg;
                    while (inc_arg < inc_end && *inc_arg != close)
                        inc_arg++;
                    spec = pp_strndup(start, inc_arg - start);
                } else {
                    char *arg_str = pp_strndup(inc_arg, inc_end - inc_arg);
                    expanded = expand_text(arg_str, filename, 1, 0);
                    if (expanded) {
                        char *ep = expanded;
                        while (*ep && isspace((unsigned char)*ep)) ep++;
                        if (*ep == '"' || *ep == '<') {
                            char close = (*ep == '"') ? '"' : '>';
                            char *start = ++ep;
                            while (*ep && *ep != close) ep++;
                            spec = pp_strndup(start, ep - start);
                        }
                    }
                }
                if (spec) {
                    char *path = resolve_include(fpath, spec);
                    if (path) {
                        char *inc = read_pp_file(path);
                        if (inc) {
                            int incl_lines = 0;
                            for (char *ip = inc; *ip; ip++)
                                if (*ip == '\n') incl_lines++;
                            unsigned src_resume = line_no + 1; // source line after #include
                            sb_puts(&out, format("# %u \"%s\"\n", line_no + 1, spec));
                            SplicedInput spliced_inc = splice_lines_with_counts(inc);
                            sb_puts(&out, preprocess_file(full_path(path), spliced_inc.text, spliced_inc.line_counts));
                            line_no += incl_lines;
                            sb_puts(&out, format("# %u \"%s\"\n", src_resume, fpath));
                        } else {
                            fprintf(stderr, "%s:%d: error: cannot read include file '%s'\n", fpath, line_no, path);
                            exit(1);
                        }
                    } else {
                        fprintf(stderr, "%s:%d: error: include file '%s' not found\n", fpath, line_no, spec);
                        exit(1);
                    }
                }
            } else if ((pp_startswith(s, "if") && !pp_startswith(s, "ifdef") && !pp_startswith(s, "ifndef"))) {
                // Handle #if - only if it's not #ifdef, #ifndef, or #elif
                // Note: check elif first since "elif" starts with "if"
                s += 2;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                char *expr = pp_strndup(s, end - s);
                long val = eval_condition(expr, filename);
                CondIncl *ci = arena_alloc(sizeof(CondIncl));
                ci->parent_active = active;
                // Only take this branch if parent is active AND condition is true
                ci->active = active && (val != 0);
                // Only mark as taken if we actually entered this branch
                ci->branch_taken = ci->active;
                ci->next = conds;
                conds = ci;
                active = ci->active;
                sb_putc(&out, '\n');
            } else if (pp_startswith(s, "elif")) {
                // #elif - check if we already took a branch
                s += 4;
                while (s < end && isspace((unsigned char)*s))
                    s++;
                if (conds) {
                    // If no branch taken yet, evaluate the condition
                    if (!conds->branch_taken) {
                        conds->active = conds->parent_active &&
                            eval_condition(trim_copy(s, end - s), filename);
                        if (conds->active)
                            conds->branch_taken = true;
                    } else {
                        // Already took a branch, this #elif is inactive
                        conds->active = false;
                    }
                    active = conds->active;
                } else {
                    // #elif without prior #if - treat as error but handle gracefully
                    active = false;
                }
                sb_putc(&out, '\n');
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
                sb_putc(&out, '\n');
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
                sb_putc(&out, '\n');
            } else if (pp_startswith(s, "else")) {
                if (conds) {
                    conds->active = conds->parent_active && !conds->branch_taken;
                    conds->branch_taken = true;
                    active = conds->active;
                }
                sb_putc(&out, '\n');
            } else if (pp_startswith(s, "endif")) {
                if (conds) {
                    CondIncl *next = conds->next;
                    active = conds->parent_active;
                    conds = next;
                }
                sb_putc(&out, '\n');
            } else {
                // Catch-all: #define, #undef, #include, #pragma, etc.
                // in inactive sections. Output newline for line counting.
                int n = line_counts ? line_counts[line_idx] : 1;
                for (int i = 0; i < n; i++)
                    sb_putc(&out, '\n');
            }
        } else if (active) {
            if (acc.len > 0 || count_unmatched_parens(line, end) > 0) {
                if (acc.len == 0) {
                    acc_line_no = line_no;
                }
                if (acc.len > 0)
                    sb_putc(&acc, ' ');
                sb_reserve(&acc, acc.len + (end - line) + 1);
                memcpy(acc.buf + acc.len, line, end - line);
                acc.len += end - line;
                acc.buf[acc.len] = '\0';
                acc_line_count++;
                acc_phys_count += line_counts ? line_counts[line_idx] : 1;
                if (count_unmatched_parens(acc.buf, acc.buf + acc.len) <= 0) {
                    char *expanded = expand_text(acc.buf, filename, acc_line_no, 0);
                    sb_puts(&out, expanded);
                    for (int i = 0; i < acc_phys_count; i++)
                        sb_putc(&out, '\n');
                    acc.len = 0;
                    acc.buf[0] = '\0';
                    acc_line_count = 0;
                    acc_phys_count = 0;
                }
            } else {
                char *expanded = expand_text(pp_strndup(line, end - line), filename, line_no, 0);
                sb_puts(&out, expanded);
                {
                    int n = line_counts ? line_counts[line_idx] : 1;
                    for (int i = 0; i < n; i++)
                        sb_putc(&out, '\n');
                }
            }
        } else {
            if (acc.len > 0) {
                /* flush accumulator before inactive section */
                char *expanded = expand_text(acc.buf, filename, acc_line_no, 0);
                sb_puts(&out, expanded);
                for (int i = 0; i < acc_phys_count; i++)
                    sb_putc(&out, '\n');
                acc.len = 0;
                acc.buf[0] = '\0';
                acc_line_count = 0;
                acc_phys_count = 0;
            }
            {
                int n = line_counts ? line_counts[line_idx] : 1;
                for (int i = 0; i < n; i++)
                    sb_putc(&out, '\n');
            }
        }

        if (line_counts)
            line_no += line_counts[line_idx++];
        else
            line_no++;
    }

    if (acc.len > 0) {
        char *expanded = expand_text(acc.buf, filename, acc_line_no, 0);
        sb_puts(&out, expanded);
        for (int i = 0; i < acc_phys_count; i++)
            sb_putc(&out, '\n');
    }

    return out.buf;
}

char *preprocess(char *filename, char *p) {
    clear_macros();
    static bool macros_inited = false;
    static char *builtin_expect_params[] = {"x", "y"};

    if (!macros_inited) {
#define define_pre(name, value) define_macro(name, false, NULL, 0, value)
        // Add builtin macros first - BEFORE calling preprocess_file
        define_pre("__has_include", "1");
        define_pre("__has_include_next", "1");

#include "gcc_predefined.h"

#if 0
    // OLD: GCC builtin type macros (required for <stdatomic.h> and other system headers)
    define_pre("__SIZE_TYPE__", "long unsigned int");
    define_pre("__PTRDIFF_TYPE__", "long int");
    define_macro("__WCHAR_TYPE__", false, NULL, 0,
#ifdef _WIN32
                     "unsigned short"
#else
                     "unsigned int"
#endif
        );
    define_pre("__WINT_TYPE__", "unsigned int");
    // GCC atomic memory order builtins (required for <stdatomic.h>)
    define_pre("__ATOMIC_RELAXED", "0");
    define_pre("__ATOMIC_CONSUME", "1");
    define_pre("__ATOMIC_ACQUIRE", "2");
    define_pre("__ATOMIC_RELEASE", "3");
    define_pre("__ATOMIC_ACQ_REL", "4");
    define_pre("__ATOMIC_SEQ_CST", "5");
    // Integer type width macros
    define_pre("__INT8_TYPE__", "signed char");
    define_pre("__INT16_TYPE__", "short int");
    define_pre("__INT32_TYPE__", "int");
    define_pre("__INT64_TYPE__", "long int");
    define_pre("__UINT8_TYPE__", "unsigned char");
    define_pre("__UINT16_TYPE__", "short unsigned int");
    define_pre("__UINT32_TYPE__", "unsigned int");
    define_pre("__UINT64_TYPE__", "long unsigned int");
    define_pre("__CHAR8_TYPE__", "unsigned char");
    define_pre("__CHAR16_TYPE__", "short unsigned int");
    define_pre("__CHAR32_TYPE__", "unsigned int");
    define_pre("__INTMAX_TYPE__", "long int");
    define_pre("__UINTMAX_TYPE__", "long unsigned int");
    define_pre("__INTPTR_TYPE__", "long int");
    define_pre("__UINTPTR_TYPE__", "long unsigned int");
    define_pre("__INT_LEAST8_TYPE__", "signed char");
    define_pre("__INT_LEAST16_TYPE__", "short int");
    define_pre("__INT_LEAST32_TYPE__", "int");
    define_pre("__INT_LEAST64_TYPE__", "long int");
    define_pre("__UINT_LEAST8_TYPE__", "unsigned char");
    define_pre("__UINT_LEAST16_TYPE__", "short unsigned int");
    define_pre("__UINT_LEAST32_TYPE__", "unsigned int");
    define_pre("__UINT_LEAST64_TYPE__", "long unsigned int");
    define_pre("__INT_FAST8_TYPE__", "signed char");
    define_pre("__INT_FAST16_TYPE__", "long int");
    define_pre("__INT_FAST32_TYPE__", "long int");
    define_pre("__INT_FAST64_TYPE__", "long int");
    define_pre("__UINT_FAST8_TYPE__", "unsigned char");
    define_pre("__UINT_FAST16_TYPE__", "long unsigned int");
    define_pre("__UINT_FAST32_TYPE__", "long unsigned int");
    define_pre("__UINT_FAST64_TYPE__", "long unsigned int");
    define_pre("__SIG_ATOMIC_TYPE__", "int");
#ifdef _WIN32
    if (!find_macro("_WIN32"))
        define_macro("_WIN32", false, NULL, 0, "1");
    if (!find_macro("__LLP64__"))
        define_macro("__LLP64__", false, NULL, 0, "1");
#endif
#ifdef __linux__
    if (!find_macro("__linux__"))
        define_macro("__linux__", false, NULL, 0, "1");
#endif
#ifdef __APPLE__
    if (!find_macro("__APPLE__"))
        define_macro("__APPLE__", false, NULL, 0, "1");
    if (!find_macro("__leading_underscore"))
        define_macro("__leading_underscore", false, NULL, 0, "1");
    if (!find_macro("__MACH__"))
        define_macro("__MACH__", false, NULL, 0, "1");
#endif
#ifdef __FreeBSD__
    if (!find_macro("__FreeBSD__"))
        define_macro("__FreeBSD__", false, NULL, 0, "1");
#endif
#ifdef __NetBSD__
    if (!find_macro("__NetBSD__"))
        define_macro("__NetBSD__", false, NULL, 0, "1");
#endif
#ifdef __OpenBSD__
    if (!find_macro("__OpenBSD__"))
        define_macro("__OpenBSD__", false, NULL, 0, "1");
#endif
#ifdef __DragonFly__
    if (!find_macro("__DragonFly__"))
        define_macro("__DragonFly__", false, NULL, 0, "1");
#endif
#if !defined(_WIN32)
    define_pre("__unix", "1");
    define_pre("__unix__", "1");
    define_pre("__LP64__", "1");
#endif
    define_macro("__builtin_expect", true, builtin_expect_params, 2, "x");
    define_pre("__builtin_abort", "abort");
    define_pre("__builtin_malloc", "malloc");
    define_pre("__builtin_calloc", "calloc");
    define_pre("__builtin_realloc", "realloc");
    define_pre("__builtin_free", "free");
    // define_pre("__builtin_memcpy", "memcpy");
    // define_pre("__builtin_memcmp", "memcmp");
    define_pre("__builtin_memmove", "memmove");
    // define_pre("__builtin_memset", "memset");
    // define_pre("__builtin_strlen", "strlen");
    // define_pre("__builtin_strcpy", "strcpy");
    define_pre("__builtin_strncpy", "strncpy");
    // define_pre("__builtin_strcmp", "strcmp");
    define_pre("__builtin_strncmp", "strncmp");
    define_pre("__builtin_strcat", "strcat");
    define_pre("__builtin_strncat", "strncat");
    // define_pre("__builtin_strchr", "strchr");
    define_pre("__builtin_strrchr", "strrchr");
    define_pre("__builtin_strdup", "strdup");
    define_pre("__builtin_alloca", "alloca");
    define_macro("__builtin_unreachable", true, NULL, 0, "while(1){}");
    // __builtin_va_* are handled as parser builtins, not macros
    define_pre("__GNUC__", "4");
    define_pre("__GNUC_MINOR__", "0");
    define_pre("__STDC__", "1");
    define_pre("__STDC_VERSION__", "201112L");
    define_pre("__extension__", "");
    // __builtin_va_list is injected as a typedef at parse time
    // Don't define __asm__ or __volatile__ as macros — the parser
    // handles __asm__, __asm, and asm directly.  Expanding them here
    // would strip the leading underscores and break the token-based
    // is_asm_keyword() detection in the parser.
    define_pre("__BYTE_ORDER__", "1234");
    define_pre("__CHAR_BIT__", "8");
    define_pre("__INT_MAX__", "2147483647");
    define_pre("__LONG_MAX__", "2147483647L");
    define_pre("__LONG_LONG_MAX__", "9223372036854775807LL");
    define_pre("__SIZEOF_INT__", "4");
    define_pre("__SIZEOF_LONG__", "4");
    define_pre("__SIZEOF_LONG_LONG__", "8");
    define_pre("__SIZEOF_POINTER__", "8");
    define_pre("__SIZEOF_FLOAT__", "4");
    define_pre("__SIZEOF_DOUBLE__", "8");
#endif

#ifdef _WIN32
        if (!find_macro("__LLP64__"))
            define_pre("__LLP64__", "1");
#else
        if (!find_macro("__LP64__"))
            define_pre("__LP64__", "1");
#endif
        define_macro("__builtin_expect", true, builtin_expect_params, 2, "x");
        define_pre("__builtin_abort", "abort");
        define_pre("__builtin_malloc", "malloc");
        define_pre("__builtin_calloc", "calloc");
        define_pre("__builtin_realloc", "realloc");
        define_pre("__builtin_free", "free");
        /* those are detected and replaced by builtins. TODO: vice-versa */
        define_pre("__builtin_memcpy", "memcpy");
        define_pre("__builtin_memcmp", "memcmp");
        define_pre("__builtin_memset", "memset");
        define_pre("__builtin_strlen", "strlen");
        define_pre("__builtin_strcpy", "strcpy");
        define_pre("__builtin_strcmp", "strcmp");
        /* those not. TODO why define them then? */
        define_pre("__builtin_memmove", "memmove");
        define_pre("__builtin_strncpy", "strncpy");
        define_pre("__builtin_strncmp", "strncmp");
        define_pre("__builtin_strcat", "strcat");
        define_pre("__builtin_strncat", "strncat");
        define_pre("__builtin_strchr", "strchr");
        define_pre("__builtin_strrchr", "strrchr");
        define_pre("__builtin_strdup", "strdup");
        define_pre("__builtin_alloca", "alloca");
        define_macro("__builtin_unreachable", true, NULL, 0, "while(1){}");
        // __builtin_va_* are handled as parser builtins, not macros
        define_pre("__extension__", "");
        // __builtin_va_list is injected as a typedef at parse time
        // Don't define __asm__ or __volatile__ as macros — the parser
        // handles __asm__, __asm, and asm directly.  Expanding them here
        // would strip the leading underscores and break the token-based
        // is_asm_keyword() detection in the parser.
#undef define_pre
        saved_macros = macros;
        macros_inited = true;
    }

    SplicedInput spliced = splice_lines_with_counts(p);
    char *result = preprocess_file(canonical_path(filename), spliced.text, spliced.line_counts);
    if (opt_dM)
        return dump_macros();
    return result;
}

void print_search_dirs(const char *gcc) {
    printf("install: %s\n", RCC_INCDIR);
    for (int i = 0; i < nb_user_include_paths; i++)
        printf("include: =%s\n", user_include_paths[i]);
    for (int i = 0; sys_include_paths[i]; i++)
        printf("include: =%s\n", sys_include_paths[i]);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -print-search-dirs 2>/dev/null", gcc);
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[4096];
        while (fgets(line, sizeof(line), fp)) {
            if (!strncmp(line, "libraries:", 10))
                fputs(line, stdout);
        }
        pclose(fp);
    }
}
