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
    TK_IDENT, // Identifiers
    TK_PUNCT, // Punctuators
    TK_NUM, // Numeric literals
    TK_FNUM, // Floating-point literals
    TK_STR, // String literals
    TK_EOF, // End of file
} TokenKind;

// Token type
typedef struct Token Token;
struct Token {
    TokenKind kind; // Token kind
    Token *next; // Next token
    int64_t val; // If kind is TK_NUM, its value
    double fval; // If kind is TK_FNUM, its value
    char *name; // If kind is TK_IDENT, its name
    char *str; // If kind is TK_STR, its contents
    char *loc; // Token location
    int len; // Token length
    // For string literals: 0 = regular, 'L' = wide, 'u' = char16_t, 'U' = char32_t
    int string_literal_prefix;
};

// Error reporting
void error(char *fmt, ...);
void error_at(char *loc, char *fmt, ...);
void error_tok(Token *tok, char *fmt, ...);
void warn_tok(Token *tok, char *fmt, ...);

// Allocator / Utils
void *arena_alloc(size_t size);
char *format(char *fmt, ...);
char *str_intern(const char *start, int len);
char *path_basename(char *path);

// Lexer entry point
char *preprocess(char *filename, char *p);
void add_define(char *def);
void add_undef(char *name);
void add_include_path(const char *path);
Token *tokenize(char *filename, char *p);
void init_builtins(void);

//
// Parser
//

// Type System
typedef enum { QUAL_CONST = 1,
               QUAL_VOLATILE = 2,
               QUAL_RESTRICT = 4 } TypeQual;

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
    int bit_width; // 0 = not a bitfield
    int bit_offset; // bit position within the storage unit
};

struct Type {
    TypeKind kind;
    int size; // sizeof
    int align; // alignment
    bool is_unsigned;
    bool is_enum; // enum type — treated as unsigned for bitfield extraction
    bool is_signed_char; // signed char vs plain char (both have is_unsigned=false)
    unsigned char qual; // TypeQual flags: const/volatile/restrict
    Type *base; // for pointer/array
    Member *members; // for struct
    Type *return_ty; // for function
    Type *param_types; // linked list of parameter types (for function)
    Type *param_next; // next in parameter type list
    bool is_variadic; // for function
    int pack_align; // #pragma pack(n) alignment, 0 = default
    char *cleanup_func; // __attribute__((__cleanup__(func))) on the type
};

static inline bool ty_const(const Type *t) { return t->qual & QUAL_CONST; }
static inline bool ty_volatile(const Type *t) { return t->qual & QUAL_VOLATILE; }
static inline bool ty_restrict(const Type *t) { return t->qual & QUAL_RESTRICT; }

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

extern bool opt_O0;
extern int pack_align;

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
    char *asm_name; // Assembly-level name (for static locals)
    int offset;
    Type *ty;
    bool is_local;
    bool is_extern;
    bool is_function;
    bool is_static; // static local variable
    bool is_inline;
    bool is_weak;
    bool has_init;
    int64_t init_val;
    char *init_data;
    int init_size;
    Reloc *relocs;
    char *cleanup_func; // __attribute__((__cleanup__(func)))
};

typedef struct Node Node;
void add_type(Node *node);

#define MAX_ASM_OPERANDS 30

typedef struct AsmOperand AsmOperand;
struct AsmOperand {
    char constraint[16]; // e.g. "=m", "r", "=r", "=a", "+m"
    char asm_str[64]; // AT&T operand string filled by codegen
    char intel_str[64]; // Intel operand string filled by codegen
    Node *expr; // C expression for the operand
    int reg; // scratch register index (-1 if hw-reg or immediate)
    int ref_index; // >= 0: matching input — same reg as ops[ref_index]
    char hw_char; // 'a','d','S','D','c' or 0 for generic scratch
    bool is_memory; // 'm' in constraint
    bool is_output; // '=' or '+' in constraint
    bool is_rw; // '+' (read-write) in constraint
    bool is_imm; // pure immediate (no register allocated)
};

typedef enum {
    ND_ADD, // +
    ND_SUB, // -
    ND_MUL, // *
    ND_DIV, // /
    ND_MOD, // %
    ND_SHL, // <<
    ND_SHR, // >>
    ND_BITAND, // &
    ND_BITXOR, // ^
    ND_BITOR, // |
    ND_EQ, // ==
    ND_NE, // !=
    ND_LT, // <
    ND_LE, // <=
    ND_ASSIGN, // =
    ND_POST_INC, // postfix ++
    ND_POST_DEC, // postfix --
    ND_ADDR, // &
    ND_DEREF, // *
    ND_CAST, // cast
    ND_BITNOT, // ~
    ND_FUNCALL, // Function call
    ND_LVAR, // Local variable
    ND_NUM, // Integer
    ND_RETURN, // "return"
    ND_IF, // "if"
    ND_FOR, // "for" or "while"
    ND_DO, // "do"
    ND_SWITCH, // "switch"
    ND_CASE, // "case" or "default"
    ND_BREAK, // "break"
    ND_CONTINUE, // "continue"
    ND_GOTO, // "goto"
    ND_GOTO_IND, // "goto *expr" (computed goto)
    ND_LABEL, // label:
    ND_LABEL_VAL, // &&label (label address)
    ND_STMT_EXPR, // GNU statement expression
    ND_BLOCK, // { ... }
    ND_EXPR_STMT, // Expression statement
    ND_NULL, // Empty statement
    ND_STR, // String literal
    ND_MEMBER, // Struct member access
    ND_LOGAND, // &&
    ND_LOGOR, // ||
    ND_COND, // ?:
    ND_COMMA, // ,
    ND_SIZEOF, // sizeof
    ND_FNUM, // Float literal
    ND_NEG, // Unary minus
    ND_NOT, // Logical not
    ND_ZERO_INIT, // Zero-fill a local variable (lhs=ND_LVAR)
    ND_ASM, // inline asm statement
} NodeKind;

typedef struct Node Node;

struct Node {
    NodeKind kind; // Node kind
    Node *next; // Next node (for blocks or statements)

    Token *tok; // Representative token for this node
    Type *ty; // AST node type

    Node *lhs; // Left-hand side
    Node *rhs; // Right-hand side

    // "if" or "for" statement
    Node *cond;
    Node *then;
    Node *els;
    Node *init;
    Node *inc;

    // Block or arguments
    Node *body;
    Node *args; // Linked list of args
    Node *stmt_expr_result;

    // Function call
    char *funcname;
    char *label_name;

    // String literal
    char *str;
    int str_id;

    // Local variable
    LVar *var;

    // Cleanup range for control-flow that exits scopes
    LVar *cleanup_begin;
    LVar *cleanup_end;
    LVar *continue_cleanup_end;

    int64_t val; // Used if kind == ND_NUM
    double fval; // Used if kind == ND_FNUM
    int array_len; // Used if kind == TY_ARRAY

    // Struct member access
    Member *member;

    // switch/case
    Node *case_next;
    Node *default_case;
    int64_t case_val;
    int64_t case_end; // for case ranges (GNU extension)
    bool is_case_range;
    int label_id;

    // ND_ASM (inline asm statement)
    char *asm_template; // raw template string (with escape sequences decoded)
    int asm_nout; // number of output operands
    int asm_noperands; // outputs + inputs
    AsmOperand *asm_ops; // [0..noperands-1], outputs first
    char **asm_goto_labels; // goto label names
    int asm_ngoto;
};

typedef struct Function Function;
struct Function {
    Function *next;
    char *name;
    char *asm_name;
    Type *ty;
    LVar *params;
    LVar *locals;
    Node *body;
    int stack_size;
    bool is_variadic;
    bool is_constructor;
    bool is_destructor;
    bool is_inline;
    bool is_static;
    bool is_extern;
    bool is_weak;
    bool has_def;
};

typedef struct StrLit StrLit;
struct StrLit {
    StrLit *next;
    char *str;
    int id;
    int prefix; // 0 = regular, 'L' = wide, 'u' = char16_t, 'U' = char32_t
    int elem_size; // size of each character element (1 for regular, 2 or 4 for wide)
    int len; // actual byte length of string content (includes embedded NULs)
    int wchar_count; // number of Unicode characters (for wide strings)
};

typedef struct TLItem TLItem;
struct TLItem {
    enum { TL_FUNC,
           TL_ASM } kind;
    Function *fn; // valid if kind == TL_FUNC
    char *asm_str; // valid if kind == TL_ASM
    TLItem *next;
};

typedef struct Program Program;
struct Program {
    TLItem *items;
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

// Unicode identifiers
uint32_t decode_utf8(char **new_pos, char *p);
bool is32_ident1(uint32_t c);
bool is32_ident2(uint32_t c);
int utf8_len(char *str);

#endif // RCC_H
