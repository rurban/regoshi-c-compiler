#include "rcc.h"
#include <stdarg.h>
#include <ctype.h>

// Input string
static char *current_input;
static char *current_filename;

// Reports an error and exit.
void error(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "\033[1;31merror:\033[0m ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    exit(1);
}

// Gorgeous error reporting with pointing carets.
static void verror_at(char *loc, int len, char *fmt, va_list ap) {
    // Find line containing `loc`
    char *line = loc;
    while (current_input < line && line[-1] != '\n')
        line--;

    char *end = loc;
    while (*end != '\n' && *end != '\0')
        end++;

    int line_num = 1;
    for (char *p = current_input; p < line; p++)
        if (*p == '\n')
            line_num++;

    // Print filename and line info
    fprintf(stderr, "\033[1;37m%s:%d: \033[0m", current_filename, line_num);
    fprintf(stderr, "\033[1;31merror:\033[0m ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");

    // Print the line
    fprintf(stderr, " %.*s\n", (int)(end - line), line);

    // Print the squiggly line
    int pos = loc - line + 1; // +1 for the space we added above
    for (int i = 0; i < pos; i++) fprintf(stderr, " "); // Indent
    
    int tilde_len = len > 0 ? len : 1;
    fprintf(stderr, "\033[1;31m^");
    for(int i = 1; i < tilde_len; i++) fprintf(stderr, "~");
    fprintf(stderr, "\033[0m\n");

    exit(1);
}

// Reports an error location and exit.
void error_at(char *loc, char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(loc, 1, fmt, ap);
}

void error_tok(Token *tok, char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    verror_at(tok->loc, tok->len, fmt, ap);
}

// Create a new token.
static Token *new_token(TokenKind kind, char *start, char *end) {
    Token *tok = arena_alloc(sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = end - start;
    return tok;
}

static bool startswith(char *p, char *q) {
    return strncmp(p, q, strlen(q)) == 0;
}

// Read a punctuator token from p and returns its length.
static int read_punct(char *p) {
    if (startswith(p, "..."))
        return 3;
    if (startswith(p, "<<=") || startswith(p, ">>="))
        return 3;
    if (startswith(p, "==") || startswith(p, "!=") ||
        startswith(p, "<=") || startswith(p, ">=") ||
        startswith(p, "++") || startswith(p, "--") ||
        startswith(p, "&&") || startswith(p, "||") ||
        startswith(p, "->") || startswith(p, "<<") ||
        startswith(p, ">>") || startswith(p, "+=") ||
        startswith(p, "-=") || startswith(p, "*=") ||
        startswith(p, "/=") || startswith(p, "%=") ||
        startswith(p, "&=") || startswith(p, "|=") ||
        startswith(p, "^="))
        return 2;

    return ispunct(*p) ? 1 : 0;
}

static bool is_ident1(char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '_' || c == '$';
}

static bool is_ident2(char c) {
    return is_ident1(c) || ('0' <= c && c <= '9');
}

static char get_escape_char(char c) {
    switch (c) {
    case 'a': return '\a';
    case 'b': return '\b';
    case 'f': return '\f';
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case '0': return '\0';
    default: return c;
    }
}

static int from_octal(char c) {
    return c - '0';
}

static int from_hex(char c) {
    if ('0' <= c && c <= '9')
        return c - '0';
    if ('a' <= c && c <= 'f')
        return c - 'a' + 10;
    if ('A' <= c && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static char read_escaped_char(char **new_pos, char *p) {
    if (*p == 'x') {
        p++;
        int val = 0;
        int digit = from_hex(*p);
        if (digit < 0)
            error_at(p, "invalid hex escape");
        while ((digit = from_hex(*p)) >= 0) {
            val = val * 16 + digit;
            p++;
        }
        *new_pos = p;
        return (char)val;
    }

    if ('0' <= *p && *p <= '7') {
        int val = 0;
        int n = 0;
        while (n < 3 && '0' <= *p && *p <= '7') {
            val = val * 8 + from_octal(*p);
            p++;
            n++;
        }
        *new_pos = p;
        return (char)val;
    }

    char c = get_escape_char(*p);
    *new_pos = p + 1;
    return c;
}

// Tokenize a given string and return new tokens.
Token *tokenize(char *filename, char *p) {
    p = preprocess(filename, p);
    current_input = p;
    current_filename = filename;
    Token head = {};
    Token *cur = &head;

    while (*p) {
        // Skip whitespace characters.
        if (isspace(*p)) {
            p++;
            continue;
        }

        // Skip preprocessor directives naively
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }

        // Skip line comments
        if (startswith(p, "//")) {
            p += 2;
            while (*p != '\n' && *p != '\0')
                p++;
            continue;
        }

        // Skip block comments
        if (startswith(p, "/*")) {
            char *q = strstr(p + 2, "*/");
            if (!q)
                error_at(p, "unclosed block comment");
            p = q + 2;
            continue;
        }

        // Numeric literal (integer or floating point)
        if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
            char *q = p;
            bool is_float = false;

            if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                while (isxdigit(*p)) p++;
                if (*p == '.') {
                    is_float = true;
                    p++;
                    while (isxdigit(*p)) p++;
                }
                if (*p == 'p' || *p == 'P') {
                    is_float = true;
                    p++;
                    if (*p == '+' || *p == '-') p++;
                    while (isdigit(*p)) p++;
                }
            } else if (*p == '0' && (p[1] == 'b' || p[1] == 'B')) {
                p += 2;
                while (*p == '0' || *p == '1') p++;
            } else {
                while (isdigit(*p)) p++;
                if (*p == '.' && p[1] != '.') {
                    is_float = true;
                    p++;
                    while (isdigit(*p)) p++;
                }
                if (*p == 'e' || *p == 'E') {
                    is_float = true;
                    p++;
                    if (*p == '+' || *p == '-') p++;
                    while (isdigit(*p)) p++;
                }
            }

            // Check for float suffix
            if (*p == 'f' || *p == 'F') {
                is_float = true;
                p++;
            } else if (*p == 'l' || *p == 'L') {
                if (is_float) {
                    p++;
                } else {
                    // Integer suffix
                    while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L')
                        p++;
                }
            } else {
                while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L')
                    p++;
            }

            if (is_float) {
                cur = cur->next = new_token(TK_FNUM, q, p);
                // Check if 'f'/'F' suffix was present (p-1 would be 'f' or 'F')
                char last = *(p - 1);
                if (last == 'f' || last == 'F') {
                    cur->fval = (double)strtof(q, NULL);
                    cur->val = 1; // flag: is single-precision float
                } else {
                    cur->fval = strtod(q, NULL);
                    cur->val = 0; // flag: is double
                }
            } else {
                cur = cur->next = new_token(TK_NUM, q, p);
                if (q[0] == '0' && (q[1] == 'b' || q[1] == 'B')) {
                    int64_t val = 0;
                    char *bp = q + 2;
                    while (*bp == '0' || *bp == '1') {
                        val = val * 2 + (*bp - '0');
                        bp++;
                    }
                    cur->val = val;
                } else {
                    cur->val = strtoull(q, NULL, 0);
                }
            }
            cur->len = (int)(p - q);
            continue;
        }

        // Identifier or keyword
        if (is_ident1(*p)) {
            char *start = p;
            do {
                p++;
            } while (is_ident2(*p));
            cur = cur->next = new_token(TK_IDENT, start, p);
            cur->name = str_intern(start, p - start);
            continue;
        }

        // String literal
        if ((*p == 'L' || *p == 'u' || *p == 'U') && p[1] == '"')
            p++;

        if (*p == '"') {
            char *start = p;
            p++;
            char *buf = arena_alloc(2048); // Pre-allocate scratch buffer
            int len = 0;
            while (*p && *p != '"') {
                if (*p == '\\') {
                    p++;
                    buf[len++] = read_escaped_char(&p, p);
                } else {
                    buf[len++] = *p++;
                }
            }
            if (!*p) error_at(start, "unclosed string literal");
            p++;
            buf[len] = '\0';
            cur = cur->next = new_token(TK_STR, start, p);
            cur->str = str_intern(buf, len); // intern it
            continue;
        }

        // Character literal
        if ((*p == 'L' || *p == 'u' || *p == 'U') && p[1] == '\'')
            p++;

        if (*p == '\'') {
            char *start = p;
            p++;
            char c;
            if (*p == '\\') {
                p++;
                c = read_escaped_char(&p, p);
            } else {
                c = *p++;
            }
            if (*p != '\'') error_at(start, "unclosed character literal");
            p++;
            cur = cur->next = new_token(TK_NUM, start, p);
            cur->val = (uint8_t)c;
            continue;
        }

        // Punctuators
        int punct_len = read_punct(p);
        if (punct_len) {
            cur = cur->next = new_token(TK_PUNCT, p, p + punct_len);
            p += punct_len;
            continue;
        }

        error_at(p, "invalid token");
    }

    cur = cur->next = new_token(TK_EOF, p, p);
    return head.next;
}
