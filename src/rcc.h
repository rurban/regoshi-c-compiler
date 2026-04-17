#ifndef RCC_H
#define RCC_H

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <float.h>

//
// Tokenizer / Lexer
//

typedef enum {
    TK_IDENT,   // Identifiers
    TK_PUNCT,   // Punctuators
    TK_NUM,     // Numeric literals
    TK_FNUM,    // Floating-point literals
    TK_STR,     // String literals
    TK_EOF,     // End of file
} TokenKind;

// Token type
typedef struct Token Token;
struct Token {
    TokenKind kind; // Token kind
    Token *next;    // Next token
    int64_t val;   // If kind is TK_NUM, its value
    double fval;   // If kind is TK_FNUM, its value
    char *name;    // If kind is TK_IDENT, its name
    char *str;     // If kind is TK_STR, its contents
    char *loc;     // Token location
    int len;        // Token length
};

// Error reporting
void error(char *fmt, ...);
void error_at(char *loc, char *fmt, ...);
void error_tok(Token *tok, char *fmt, ...);

// Allocator / Utils
void *arena_alloc(size_t size);
char *format(char *fmt, ...);
char *str_intern(char *start, int len);

// Lexer entry point
char *preprocess(char *filename, char *p);
Token *tokenize(char *filename, char *p);

//
// Parser
//

// Type System
typedef enum {
    TY_VOID,
    TY_BOOL,
    TY_INT,
    TY_CHAR,
    TY_SHORT,
    TY_LONG,
    TY_LLONG,
    TY_FLOAT,
    TY_DOUBLE,
    TY_LDOUBLE,
    TY_PTR,
    TY_ARRAY,
    TY_STRUCT,
    TY_UNION,
    TY_FUNC,
} TypeKind;

typedef struct Type Type;
typedef struct Member Member;

struct Member {
    Member *next;
    Type *ty;
    char *name;
    int offset;
};

struct Type {
    TypeKind kind;
    int size;   // sizeof
    int align;  // alignment
    bool is_unsigned;
    Type *base; // for pointer/array
    Member *members; // for struct
    Type *return_ty; // for function
    Type *param_types; // linked list of parameter types (for function)
    Type *param_next;  // next in parameter type list
    bool is_variadic;  // for function
};

typedef struct Typedef Typedef;
struct Typedef {
    Typedef *next;
    char *name;
    Type *ty;
};

extern Type *ty_void;
extern Type *ty_bool;
extern Type *ty_int;
extern Type *ty_uint;
extern Type *ty_char;
extern Type *ty_uchar;
extern Type *ty_short;
extern Type *ty_ushort;
extern Type *ty_long;
extern Type *ty_ulong;
extern Type *ty_llong;
extern Type *ty_ullong;
extern Type *ty_float;
extern Type *ty_double;
extern Type *ty_ldouble;

bool is_integer(Type *ty);
bool is_flonum(Type *ty);
bool is_number(Type *ty);
Type *get_integer_type(int size, bool is_unsigned);
Type *pointer_to(Type *base);
Type *array_of(Type *base, int len);

typedef struct Reloc Reloc;
struct Reloc {
    Reloc *next;
    int offset;
    char *label;
    int addend;
};

typedef struct LVar LVar;
struct LVar {
    LVar *next;
    LVar *param_next;
    char *name;
    char *asm_name;    // Assembly-level name (for static locals)
    int offset;
    Type *ty;
    bool is_local;
    bool is_extern;
    bool is_function;
    bool is_static;    // static local variable
    bool has_init;
    int64_t init_val;
    char *init_data;
    int init_size;
    Reloc *relocs;
};

typedef struct Node Node;
void add_type(Node *node);

typedef enum {
    ND_ADD,       // +
    ND_SUB,       // -
    ND_MUL,       // *
    ND_DIV,       // /
    ND_MOD,       // %
    ND_SHL,       // <<
    ND_SHR,       // >>
    ND_BITAND,    // &
    ND_BITXOR,    // ^
    ND_BITOR,     // |
    ND_EQ,        // ==
    ND_NE,        // !=
    ND_LT,        // <
    ND_LE,        // <=
    ND_ASSIGN,    // =
    ND_POST_INC,  // postfix ++
    ND_POST_DEC,  // postfix --
    ND_ADDR,      // &
    ND_DEREF,     // *
    ND_CAST,      // cast
    ND_BITNOT,    // ~
    ND_FUNCALL,   // Function call
    ND_LVAR,      // Local variable
    ND_NUM,       // Integer
    ND_RETURN,    // "return"
    ND_IF,        // "if"
    ND_FOR,       // "for" or "while"
    ND_DO,        // "do"
    ND_SWITCH,    // "switch"
    ND_CASE,      // "case" or "default"
    ND_BREAK,     // "break"
    ND_CONTINUE,  // "continue"
    ND_GOTO,      // "goto"
    ND_LABEL,     // label:
    ND_STMT_EXPR, // GNU statement expression
    ND_BLOCK,     // { ... }
    ND_EXPR_STMT, // Expression statement
    ND_NULL,      // Empty statement
    ND_STR,       // String literal
    ND_MEMBER,    // Struct member access
    ND_LOGAND,    // &&
    ND_LOGOR,     // ||
    ND_COND,      // ?:
    ND_COMMA,     // ,
    ND_SIZEOF,    // sizeof
    ND_FNUM,      // Float literal
    ND_NEG,       // Unary minus
    ND_NOT,       // Logical not
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind kind; // Node kind
    Node *next;    // Next node (for blocks or statements)

    Token *tok;    // Representative token for this node
    Type *ty;      // AST node type

    Node *lhs;     // Left-hand side
    Node *rhs;     // Right-hand side

    // "if" or "for" statement
    Node *cond;
    Node *then;
    Node *els;
    Node *init;
    Node *inc;

    // Block or arguments
    Node *body;
    Node *args; // Linked list of args

    // Function call
    char *funcname;
    char *label_name;

    // String literal
    char *str;
    int str_id;

    // Local variable
    LVar *var;

    int64_t val;   // Used if kind == ND_NUM
    double fval;   // Used if kind == ND_FNUM
    int array_len; // Used if kind == TY_ARRAY

    // Struct member access
    Member *member;

    // switch/case
    Node *case_next;
    Node *default_case;
    int64_t case_val;
    int64_t case_end;  // for case ranges (GNU extension)
    bool is_case_range;
    int label_id;
};

typedef struct Function Function;
struct Function {
    Function *next;
    char *name;
    Type *ty;
    LVar *params;
    Node *body;
    int stack_size;
    bool is_variadic;
};

typedef struct StrLit StrLit;
struct StrLit {
    StrLit *next;
    char *str;
    int id;
};

typedef struct Program Program;
struct Program {
    Function *funcs;
    LVar *globals;
    StrLit *strs;
};

// Parser entry point
Program *parse(Token *tok);

//
// CodeGen
//
void codegen(Program *prog);

// Optimizer (CTFE)
void optimize(Program *prog);

#endif // RCC_H
