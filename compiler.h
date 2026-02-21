#ifndef COMPILER_H
#define COMPILER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <assert.h>

/* ========== TOKEN TYPES ========== */
typedef enum {
    /* Literals */
    TOK_INT_LIT, TOK_FLOAT_LIT, TOK_CHAR_LIT, TOK_STRING_LIT,
    /* Identifiers */
    TOK_IDENT,
    /* Keywords */
    TOK_IMPORT, TOK_AS, TOK_SECTION, TOK_FLAGS,
    TOK_FN, TOK_GLOBAL, TOK_EXTERN, TOK_INLINE,
    TOK_VAR, TOK_CONST,
    TOK_STRUCT, TOK_UNION, TOK_ENUM, TOK_MACRO,
    TOK_ALIGN, TOK_ALLOC, TOK_EXEC, TOK_WRITE, TOK_NOLOAD,
    TOK_IF, TOK_ELSE, TOK_LOOP, TOK_WHILE, TOK_FOR, TOK_IN,
    TOK_GOTO, TOK_BREAK, TOK_CONTINUE, TOK_RETURN,
    TOK_ASM, TOK_INTEL,
    TOK_CAST, TOK_SIZEOF, TOK_ALIGNOF, TOK_OFFSETOF,
    TOK_TRUE, TOK_FALSE, TOK_NULL,
    /* Types */
    TOK_U8, TOK_U16, TOK_U32, TOK_U64,
    TOK_I8, TOK_I16, TOK_I32, TOK_I64,
    TOK_F32, TOK_F64,
    TOK_BOOL, TOK_VOID, TOK_BYTE, TOK_USIZE, TOK_ISIZE,
    TOK_MUT,
    /* Asm operand keywords */
    TOK_IN_KW, TOK_OUT_KW, TOK_CLOBBER,
    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_BANG,
    TOK_LT, TOK_GT, TOK_LSHIFT, TOK_RSHIFT, TOK_URSHIFT,
    TOK_ROL, TOK_ROR,
    TOK_EQ, TOK_NEQ, TOK_LEQ, TOK_GEQ,
    TOK_AND, TOK_OR,
    TOK_ARROW, TOK_DOT, TOK_DOTDOT, TOK_DOTDOTEQ,
    TOK_PLUS_WRAP, TOK_MINUS_WRAP, TOK_STAR_WRAP,
    /* Assign operators */
    TOK_ASSIGN,
    TOK_PLUS_ASSIGN, TOK_MINUS_ASSIGN, TOK_STAR_ASSIGN,
    TOK_SLASH_ASSIGN, TOK_PERCENT_ASSIGN,
    TOK_AMP_ASSIGN, TOK_PIPE_ASSIGN, TOK_CARET_ASSIGN,
    TOK_LSHIFT_ASSIGN, TOK_RSHIFT_ASSIGN,
    /* Punctuation */
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMICOLON, TOK_COLON, TOK_COLONCOLON,
    TOK_AT, TOK_ELLIPSIS,
    /* End of file */
    TOK_EOF,
    TOK_NEWLINE,
    TOK_COUNT
} TokenType;

typedef struct {
    TokenType type;
    char *text;       /* interned string */
    int   line;
    int   col;
    /* literal value */
    union {
        int64_t  ival;
        double   fval;
    } val;
    /* int literal suffix */
    char suffix[8];
} Token;

/* ========== LEXER ========== */
typedef struct {
    const char *src;
    int pos, line, col;
    const char *filename;
} Lexer;

/* ========== AST TYPES ========== */
typedef enum {
    /* Types */
    TY_INT, TY_FLOAT, TY_BOOL, TY_VOID, TY_BYTE,
    TY_USIZE, TY_ISIZE,
    TY_PTR, TY_MUT_PTR, TY_ARRAY, TY_FN, TY_NAMED,
    /* Expressions */
    EXPR_INT, EXPR_FLOAT, EXPR_BOOL, EXPR_NULL,
    EXPR_CHAR, EXPR_STRING,
    EXPR_IDENT,
    EXPR_UNARY, EXPR_BINARY, EXPR_ASSIGN,
    EXPR_CALL, EXPR_INDEX, EXPR_FIELD, EXPR_ARROW,
    EXPR_CAST, EXPR_SIZEOF_T, EXPR_SIZEOF_E,
    EXPR_ALIGNOF, EXPR_OFFSETOF,
    EXPR_STRUCT_INIT, EXPR_ARRAY_INIT, EXPR_ARRAY_REPEAT,
    EXPR_DIRECTIVE,
    /* Statements */
    STMT_VAR, STMT_CONST, STMT_EXPR,
    STMT_IF, STMT_LOOP, STMT_WHILE, STMT_FOR_C, STMT_FOR_RANGE,
    STMT_GOTO, STMT_LABEL,
    STMT_BREAK, STMT_CONTINUE, STMT_RETURN,
    STMT_BLOCK, STMT_ASM,
    /* Top-level */
    TOP_IMPORT, TOP_SECTION, TOP_FN, TOP_VAR, TOP_CONST,
    TOP_STRUCT, TOP_UNION, TOP_ENUM, TOP_MACRO,
    TOP_DIRECTIVE,
    /* Program */
    NODE_PROGRAM
} NodeKind;

/* Integer width/sign encoding */
typedef enum {
    INT_U8=0, INT_U16, INT_U32, INT_U64,
    INT_I8,   INT_I16, INT_I32, INT_I64
} IntKind;

typedef enum {
    FLOAT_F32, FLOAT_F64
} FloatKind;

/* Forward declare */
typedef struct AstNode AstNode;
typedef struct AstList AstList;

struct AstList {
    AstNode **items;
    int count, cap;
};

struct AstNode {
    NodeKind kind;
    int line, col;

    union {
        /* ---- Types ---- */
        struct { IntKind kind; }       ty_int;
        struct { FloatKind kind; }     ty_float;
        struct { AstNode *base; }      ty_ptr;
        struct { AstNode *base; AstNode *size_expr; } ty_array;
        struct { AstList params; AstNode *ret; } ty_fn;
        struct { char *name; }         ty_named;

        /* ---- Literals ---- */
        struct { int64_t val; IntKind kind; char suffix[8]; } int_lit;
        struct { double val; FloatKind kind; }                float_lit;
        struct { bool val; }                                  bool_lit;
        struct { int codepoint; }                             char_lit;
        struct { char *val; bool is_bytes; bool is_c; }       str_lit;

        /* ---- Expressions ---- */
        struct { char *name; }          ident;
        struct { int op; AstNode *operand; } unary;
        struct { int op; AstNode *lhs, *rhs; } binary;
        struct { int op; AstNode *lhs, *rhs; } assign;
        struct { AstNode *callee; AstList args; } call;
        struct { AstNode *base; AstNode *index; } index;
        struct { AstNode *base; char *field; } field;
        struct { AstNode *type; AstNode *expr; } cast;
        struct { AstNode *type; }        sizeof_t;
        struct { AstNode *expr; }        sizeof_e;
        struct { AstNode *type; }        alignof_;
        struct { AstNode *type; char *field; } offsetof_;
        struct { AstNode *type; AstList fields; AstList values; } struct_init;
        struct { AstList elems; }        array_init;
        struct { AstNode *elem; AstNode *count; } array_repeat;
        struct { char *name; AstList args; } directive_expr;

        /* ---- Statements ---- */
        struct {
            AstNode *type; AstNode *init;
            char *name;
            AstList attrs;
        } var_decl;
        struct {
            AstNode *type; AstNode *init;
            char *name;
        } const_decl;
        struct { AstNode *expr; } expr_stmt;
        struct {
            AstList conds; AstList blocks; AstNode *else_block;
        } if_stmt;
        struct {
            char *label; AstNode *body;
        } loop_stmt;
        struct {
            char *label; AstNode *cond; AstNode *body;
        } while_stmt;
        struct {
            char *label; char *var; AstNode *type;
            AstNode *init; AstNode *cond; AstNode *post;
            AstNode *body;
        } for_c;
        struct {
            char *label; char *var;
            AstNode *from; AstNode *to;
            bool inclusive; AstNode *body;
        } for_range;
        struct { char *target; } goto_stmt;
        struct { char *name; }   label_stmt;
        struct { char *label; }  break_stmt;
        struct { char *label; }  continue_stmt;
        struct { AstNode *expr; } return_stmt;
        struct { AstList stmts; } block;
        struct {
            bool intel;
            /* operands */
            int op_count;
            struct { int kind; /* 0=in,1=out,2=clobber */ char *reg; AstNode *expr; bool is_return; } operands[16];
            /* asm strings */
            int str_count;
            char *strings[64];
        } asm_block;

        /* ---- Top-level ---- */
        struct { char *path; char *alias; } import_decl;
        struct {
            char *name;
            int flags; /* bitmask */
            AstList items;
        } section;
        struct {
            AstList attrs;
            int modifiers; /* bitmask: GLOBAL=1,EXTERN=2,INLINE=4 */
            char *name;
            AstList params; /* each param is a var_decl node */
            bool variadic;
            AstNode *ret_type;
            AstNode *body; /* NULL if extern */
        } fn_decl;
        struct {
            AstList attrs;
            char *name; int align;
            AstList field_names;
            AstList field_types;
        } struct_decl;
        struct {
            char *name;
            AstList field_names;
            AstList field_types;
        } union_decl;
        struct {
            char *name; IntKind base_type;
            int var_count;
            char *var_names[256];
            int64_t var_values[256];
            bool var_has_val[256];
        } enum_decl;
        struct {
            char *name;
            AstList params;
            AstNode *ret_type;
            AstNode *body; /* expr or block */
        } macro_decl;
        struct { char *name; AstList args; } directive;

        /* Program */
        struct { AstList items; } program;
    };
};

/* ========== PARSER ========== */
typedef struct {
    Lexer  *lexer;
    Token   cur, peek;
    const char *filename;
    int error_count;
} Parser;

/* ========== SYMBOL TABLE ========== */
typedef enum {
    SYM_VAR, SYM_CONST, SYM_FN, SYM_STRUCT, SYM_UNION,
    SYM_ENUM, SYM_PARAM, SYM_LOCAL
} SymKind;

typedef struct Symbol Symbol;
struct Symbol {
    char    *name;
    SymKind  kind;
    AstNode *type_node;  /* type AST */
    AstNode *decl;       /* declaration AST */
    int      offset;     /* stack offset for locals */
    bool     is_global;
    Symbol  *next;       /* hash chain */
};

typedef struct Scope Scope;
struct Scope {
    Symbol  **table;
    int       size;
    Scope    *parent;
};

/* ========== CODEGEN ========== */
typedef struct {
    FILE  *out;
    Scope *scope;
    int    label_count;
    int    stack_offset;    /* current frame offset */
    int    frame_size;
    const char *current_fn;
    /* loop stack for break/continue */
    char    break_labels[64][64];
    char    cont_labels[64][64];
    int     loop_depth;
} Codegen;

/* ========== FUNCTION DECLARATIONS ========== */

/* Arena allocator */
void *arena_alloc(size_t n);
char *arena_strdup(const char *s);
void  arena_reset(void);

/* AST helpers */
AstNode *ast_node(NodeKind kind, int line, int col);
void     ast_list_push(AstList *list, AstNode *node);

/* Lexer */
Lexer *lexer_new(const char *src, const char *filename);
Token  lexer_next(Lexer *l);
void   lexer_free(Lexer *l);

/* Parser */
Parser  *parser_new(const char *src, const char *filename);
AstNode *parser_parse(Parser *p);
void     parser_free(Parser *p);

/* Type helpers */
const char *inttype_name(IntKind k);
bool inttype_signed(IntKind k);
int  inttype_bytes(IntKind k);

/* AST dump */
void ast_dump(AstNode *node, int indent);

/* Scope */
Scope  *scope_new(Scope *parent);
Symbol *scope_lookup(Scope *s, const char *name);
Symbol *scope_define(Scope *s, const char *name, SymKind kind, AstNode *decl);

/* Codegen x86-64 */
void codegen_program(Codegen *cg, AstNode *program);
Codegen *codegen_new(FILE *out);

/* Error */
void error(const char *file, int line, int col, const char *fmt, ...);
void warning(const char *file, int line, int col, const char *fmt, ...);

#endif /* COMPILER_H */
