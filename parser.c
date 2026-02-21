#include "compiler.h"

/* ===== Parser helpers ===== */

static void skip_newlines(Parser *p);

Parser *parser_new(const char *src, const char *filename) {
    Parser *p = arena_alloc(sizeof(Parser));
    p->lexer    = lexer_new(src, filename);
    p->filename = filename;
    /* prime the two-token lookahead */
    p->cur  = lexer_next(p->lexer);
    p->peek = lexer_next(p->lexer);
    return p;
}

void parser_free(Parser *p) { (void)p; }

static Token advance_tok(Parser *p) {
    Token t = p->cur;
    p->cur   = p->peek;
    p->peek  = lexer_next(p->lexer);
    return t;
}

static Token cur(Parser *p)  { return p->cur; }
static Token peek_tok(Parser *p) { return p->peek; }

static bool check(Parser *p, TokenType t) { return p->cur.type == t; }

static bool check2(Parser *p, TokenType t) { return p->peek.type == t; }

static bool match_tok(Parser *p, TokenType t) {
    if (p->cur.type == t) { advance_tok(p); return true; }
    return false;
}

static Token expect(Parser *p, TokenType t, const char *msg) {
    if (p->cur.type != t) {
        error(p->filename, p->cur.line, p->cur.col,
              "expected %s, got '%s'", msg, p->cur.text ? p->cur.text : "?");
        p->error_count++;
        /* try to recover */
    }
    return advance_tok(p);
}

static void skip_newlines(Parser *p) {
    while (p->cur.type == TOK_NEWLINE) advance_tok(p);
}

static bool eat_eos(Parser *p) {
    if (p->cur.type == TOK_SEMICOLON || p->cur.type == TOK_NEWLINE) {
        advance_tok(p); return true;
    }
    if (p->cur.type == TOK_RBRACE || p->cur.type == TOK_EOF) return true;
    error(p->filename, p->cur.line, p->cur.col, "expected ';' or newline");
    p->error_count++;
    return false;
}

/* ===== Forward declarations ===== */
static AstNode *parse_type(Parser *p);
static AstNode *parse_expr(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_stmt(Parser *p);
static AstNode *parse_top_item(Parser *p);
static AstList  parse_attr_list(Parser *p);

/* ===== Type parser ===== */

static AstNode *parse_type(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    AstNode *n;

    /* pointer */
    if (check(p, TOK_STAR)) {
        advance_tok(p);
        bool is_mut = match_tok(p, TOK_MUT);
        AstNode *base = parse_type(p);
        n = ast_node(is_mut ? TY_MUT_PTR : TY_PTR, line, col);
        n->ty_ptr.base = base;
        return n;
    }

    /* array */
    if (check(p, TOK_LBRACKET)) {
        advance_tok(p);
        AstNode *elem = parse_type(p);
        expect(p, TOK_SEMICOLON, "';'");
        AstNode *size = parse_expr(p);
        expect(p, TOK_RBRACKET, "']'");
        n = ast_node(TY_ARRAY, line, col);
        n->ty_array.base = elem;
        n->ty_array.size_expr = size;
        return n;
    }

    /* fn type */
    if (check(p, TOK_FN)) {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        n = ast_node(TY_FN, line, col);
        skip_newlines(p);
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            ast_list_push(&n->ty_fn.params, parse_type(p));
            if (!match_tok(p, TOK_COMMA)) break;
        }
        expect(p, TOK_RPAREN, "')'");
        expect(p, TOK_ARROW, "'->'");
        n->ty_fn.ret = parse_type(p);
        return n;
    }

    /* primitive types */
#define INT_TYPE(tok, k) case tok: { advance_tok(p); n = ast_node(TY_INT,line,col); n->ty_int.kind=k; return n; }
    switch (p->cur.type) {
    INT_TYPE(TOK_U8,  INT_U8)  INT_TYPE(TOK_U16, INT_U16)
    INT_TYPE(TOK_U32, INT_U32) INT_TYPE(TOK_U64, INT_U64)
    INT_TYPE(TOK_I8,  INT_I8)  INT_TYPE(TOK_I16, INT_I16)
    INT_TYPE(TOK_I32, INT_I32) INT_TYPE(TOK_I64, INT_I64)
    case TOK_F32: advance_tok(p); n=ast_node(TY_FLOAT,line,col); n->ty_float.kind=FLOAT_F32; return n;
    case TOK_F64: advance_tok(p); n=ast_node(TY_FLOAT,line,col); n->ty_float.kind=FLOAT_F64; return n;
    case TOK_BOOL: advance_tok(p); return ast_node(TY_BOOL,line,col);
    case TOK_VOID: advance_tok(p); return ast_node(TY_VOID,line,col);
    case TOK_BYTE: advance_tok(p); return ast_node(TY_BYTE,line,col);
    case TOK_USIZE: advance_tok(p); n=ast_node(TY_INT,line,col); n->ty_int.kind=INT_U64; return n;
    case TOK_ISIZE: advance_tok(p); n=ast_node(TY_INT,line,col); n->ty_int.kind=INT_I64; return n;
    case TOK_IDENT: {
        n = ast_node(TY_NAMED, line, col);
        n->ty_named.name = arena_strdup(p->cur.text);
        advance_tok(p);
        return n;
    }
    default:
        error(p->filename, line, col, "expected type, got '%s'", p->cur.text);
        p->error_count++;
        n = ast_node(TY_VOID, line, col);
        return n;
    }
#undef INT_TYPE
}

/* ===== Expression parser ===== */

static AstNode *parse_primary(Parser *p);
static AstNode *parse_unary(Parser *p);
static AstNode *parse_postfix(Parser *p);
static AstNode *parse_mul(Parser *p);
static AstNode *parse_add(Parser *p);
static AstNode *parse_shift(Parser *p);
static AstNode *parse_rel(Parser *p);
static AstNode *parse_eq(Parser *p);
static AstNode *parse_band(Parser *p);
static AstNode *parse_bxor(Parser *p);
static AstNode *parse_bor(Parser *p);
static AstNode *parse_and(Parser *p);
static AstNode *parse_or(Parser *p);
static AstNode *parse_assign(Parser *p);

static AstList parse_arg_list(Parser *p) {
    AstList list = {0};
    skip_newlines(p);
    while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
        ast_list_push(&list, parse_expr(p));
        skip_newlines(p);
        if (!match_tok(p, TOK_COMMA)) break;
        skip_newlines(p);
    }
    return list;
}

static AstNode *parse_primary(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    AstNode *n;

    switch (p->cur.type) {
    case TOK_INT_LIT: {
        n = ast_node(EXPR_INT, line, col);
        n->int_lit.val = p->cur.val.ival;
        memcpy(n->int_lit.suffix, p->cur.suffix, 8);
        advance_tok(p);
        return n;
    }
    case TOK_FLOAT_LIT: {
        n = ast_node(EXPR_FLOAT, line, col);
        n->float_lit.val = p->cur.val.fval;
        n->float_lit.kind = (p->cur.suffix[0] == 'f' && p->cur.suffix[1] == '3') ? FLOAT_F32 : FLOAT_F64;
        advance_tok(p);
        return n;
    }
    case TOK_CHAR_LIT: {
        n = ast_node(EXPR_CHAR, line, col);
        n->char_lit.codepoint = (int)p->cur.val.ival;
        advance_tok(p);
        return n;
    }
    case TOK_STRING_LIT: {
        n = ast_node(EXPR_STRING, line, col);
        n->str_lit.val = arena_strdup(p->cur.text);
        n->str_lit.is_bytes = (p->cur.suffix[0] == 'b');
        n->str_lit.is_c     = (p->cur.suffix[0] == 'c');
        advance_tok(p);
        return n;
    }
    case TOK_TRUE: advance_tok(p); n=ast_node(EXPR_BOOL,line,col); n->bool_lit.val=true; return n;
    case TOK_FALSE: advance_tok(p); n=ast_node(EXPR_BOOL,line,col); n->bool_lit.val=false; return n;
    case TOK_NULL: advance_tok(p); return ast_node(EXPR_NULL,line,col);

    case TOK_LPAREN: {
        advance_tok(p);
        n = parse_expr(p);
        expect(p, TOK_RPAREN, "')'");
        return n;
    }

    case TOK_CAST: {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        AstNode *ty = parse_type(p);
        expect(p, TOK_RPAREN, "')'");
        AstNode *expr = parse_unary(p);
        n = ast_node(EXPR_CAST, line, col);
        n->cast.type = ty;
        n->cast.expr = expr;
        return n;
    }

    case TOK_SIZEOF: {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        /* Disambiguate: if starts with a type token, treat as sizeof(type) */
        /* Simple heuristic: try type parse */
        /* For now: if IDENT followed by ')' or ',', assume type */
        /* We'll parse type for known type tokens */
        bool is_type = false;
        switch (p->cur.type) {
        case TOK_U8: case TOK_U16: case TOK_U32: case TOK_U64:
        case TOK_I8: case TOK_I16: case TOK_I32: case TOK_I64:
        case TOK_F32: case TOK_F64: case TOK_BOOL: case TOK_VOID:
        case TOK_BYTE: case TOK_USIZE: case TOK_ISIZE:
        case TOK_STAR: case TOK_LBRACKET: case TOK_FN:
            is_type = true; break;
        case TOK_IDENT:
            /* Could be type or expr — default to expr */
            is_type = false; break;
        default: is_type = false;
        }
        if (is_type) {
            AstNode *ty = parse_type(p);
            expect(p, TOK_RPAREN, "')'");
            n = ast_node(EXPR_SIZEOF_T, line, col);
            n->sizeof_t.type = ty;
        } else {
            AstNode *expr = parse_expr(p);
            expect(p, TOK_RPAREN, "')'");
            n = ast_node(EXPR_SIZEOF_E, line, col);
            n->sizeof_e.expr = expr;
        }
        return n;
    }

    case TOK_ALIGNOF: {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        AstNode *ty = parse_type(p);
        expect(p, TOK_RPAREN, "')'");
        n = ast_node(EXPR_ALIGNOF, line, col);
        n->alignof_.type = ty;
        return n;
    }

    case TOK_OFFSETOF: {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        AstNode *ty = parse_type(p);
        expect(p, TOK_COMMA, "','");
        char *field = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "field name");
        expect(p, TOK_RPAREN, "')'");
        n = ast_node(EXPR_OFFSETOF, line, col);
        n->offsetof_.type = ty;
        n->offsetof_.field = field;
        return n;
    }

    case TOK_AT: {
        advance_tok(p);
        char *name = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "identifier");
        expect(p, TOK_LPAREN, "'('");
        AstList args = parse_arg_list(p);
        expect(p, TOK_RPAREN, "')'");
        n = ast_node(EXPR_DIRECTIVE, line, col);
        n->directive_expr.name = name;
        n->directive_expr.args = args;
        return n;
    }

    /* Array literal or repeat */
    case TOK_LBRACKET: {
        advance_tok(p);
        skip_newlines(p);
        if (check(p, TOK_RBRACKET)) {
            advance_tok(p);
            n = ast_node(EXPR_ARRAY_INIT, line, col);
            return n;
        }
        AstNode *first = parse_expr(p);
        skip_newlines(p);
        if (check(p, TOK_SEMICOLON)) {
            advance_tok(p);
            AstNode *count = parse_expr(p);
            expect(p, TOK_RBRACKET, "']'");
            n = ast_node(EXPR_ARRAY_REPEAT, line, col);
            n->array_repeat.elem  = first;
            n->array_repeat.count = count;
            return n;
        }
        n = ast_node(EXPR_ARRAY_INIT, line, col);
        ast_list_push(&n->array_init.elems, first);
        skip_newlines(p);
        while (match_tok(p, TOK_COMMA)) {
            skip_newlines(p);
            if (check(p, TOK_RBRACKET)) break;
            ast_list_push(&n->array_init.elems, parse_expr(p));
            skip_newlines(p);
        }
        expect(p, TOK_RBRACKET, "']'");
        return n;
    }

    /* Struct literal: IDENT '{' field: expr, ... '}' */
    /* We handle this in postfix when we see IDENT '{' */
    case TOK_FLAGS: case TOK_IN: case TOK_ALLOC:
    case TOK_EXEC:  case TOK_WRITE: case TOK_NOLOAD:
    case TOK_INTEL: case TOK_ALIGN:
    case TOK_IDENT: {
        /* peek for struct literal: IDENT followed by '{' */
        if (check2(p, TOK_LBRACE)) {
            /* Could be a struct literal */
            /* We'll let postfix handle it as IDENT(type) then '{' */
            /* Actually parse as struct_init here */
            AstNode *ty_node = parse_type(p); /* parses IDENT */
            if (check(p, TOK_LBRACE)) {
                advance_tok(p);
                skip_newlines(p);
                n = ast_node(EXPR_STRUCT_INIT, line, col);
                n->struct_init.type = ty_node;
                while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
                    skip_newlines(p);
                    if (check(p, TOK_RBRACE)) break;
                    char *fname = arena_strdup(p->cur.text);
                    expect(p, TOK_IDENT, "field name");
                    expect(p, TOK_COLON, "':'");
                    AstNode *val = parse_expr(p);
                    ast_list_push(&n->struct_init.fields, ast_node(EXPR_IDENT, line, col)); /* dummy, store name */
                    n->struct_init.fields.items[n->struct_init.fields.count-1]->ident.name = fname;
                    ast_list_push(&n->struct_init.values, val);
                    skip_newlines(p);
                    if (!match_tok(p, TOK_COMMA)) break;
                    skip_newlines(p);
                }
                expect(p, TOK_RBRACE, "'}'");
                return n;
            }
            return ty_node; /* just the type as ident node -- shouldn't happen in expr context */
        }
        n = ast_node(EXPR_IDENT, line, col);
        n->ident.name = arena_strdup(p->cur.text);
        advance_tok(p);
        return n;
    }

    default:
        error(p->filename, line, col, "unexpected token '%s' in expression", p->cur.text ? p->cur.text : "?");
        p->error_count++;
        advance_tok(p);
        return ast_node(EXPR_NULL, line, col);
    }
}

static AstNode *parse_postfix(Parser *p) {
    AstNode *base = parse_primary(p);
    for (;;) {
        int line = p->cur.line, col = p->cur.col;
        if (check(p, TOK_DOT)) {
            advance_tok(p);
            char *field = arena_strdup(p->cur.text);
            expect(p, TOK_IDENT, "field name");
            AstNode *n = ast_node(EXPR_FIELD, line, col);
            n->field.base  = base;
            n->field.field = field;
            base = n;
        } else if (check(p, TOK_ARROW)) {
            advance_tok(p);
            char *field = arena_strdup(p->cur.text);
            expect(p, TOK_IDENT, "field name");
            AstNode *n = ast_node(EXPR_ARROW, line, col);
            n->field.base  = base;
            n->field.field = field;
            base = n;
        } else if (check(p, TOK_LBRACKET)) {
            advance_tok(p);
            AstNode *idx = parse_expr(p);
            expect(p, TOK_RBRACKET, "']'");
            AstNode *n = ast_node(EXPR_INDEX, line, col);
            n->index.base  = base;
            n->index.index = idx;
            base = n;
        } else if (check(p, TOK_LPAREN)) {
            advance_tok(p);
            AstList args = parse_arg_list(p);
            expect(p, TOK_RPAREN, "')'");
            AstNode *n = ast_node(EXPR_CALL, line, col);
            n->call.callee = base;
            n->call.args   = args;
            base = n;
        } else {
            break;
        }
    }
    return base;
}

static AstNode *parse_unary(Parser *p) {
    int line = p->cur.line, col = p->cur.col;
    AstNode *n;
    switch (p->cur.type) {
    case TOK_STAR: {
        advance_tok(p);
        AstNode *operand = parse_unary(p);
        n = ast_node(EXPR_UNARY, line, col);
        n->unary.op = TOK_STAR; n->unary.operand = operand;
        return n;
    }
    case TOK_AMP: {
        advance_tok(p);
        bool is_mut = match_tok(p, TOK_MUT);
        AstNode *operand = parse_unary(p);
        n = ast_node(EXPR_UNARY, line, col);
        n->unary.op = is_mut ? (int)'M' : TOK_AMP;
        n->unary.operand = operand;
        return n;
    }
    case TOK_MINUS: {
        advance_tok(p);
        AstNode *operand = parse_unary(p);
        n = ast_node(EXPR_UNARY, line, col);
        n->unary.op = TOK_MINUS; n->unary.operand = operand;
        return n;
    }
    case TOK_TILDE: {
        advance_tok(p);
        AstNode *operand = parse_unary(p);
        n = ast_node(EXPR_UNARY, line, col);
        n->unary.op = TOK_TILDE; n->unary.operand = operand;
        return n;
    }
    case TOK_BANG: {
        advance_tok(p);
        AstNode *operand = parse_unary(p);
        n = ast_node(EXPR_UNARY, line, col);
        n->unary.op = TOK_BANG; n->unary.operand = operand;
        return n;
    }
    default: return parse_postfix(p);
    }
}

#define BINOP(name, sub, op_a, op_b, tok_a, tok_b) \
static AstNode *name(Parser *p) { \
    AstNode *lhs = sub(p); \
    for (;;) { \
        int line=p->cur.line,col=p->cur.col; int op=0; \
        if(check(p,tok_a)){op=tok_a;advance_tok(p);} \
        else if(tok_b && check(p,tok_b)){op=tok_b;advance_tok(p);} \
        else break; \
        AstNode *rhs=sub(p); \
        AstNode *n=ast_node(EXPR_BINARY,line,col); \
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs; \
        lhs=n; \
    } return lhs; \
}

/* For multi-op levels we do it manually */
static AstNode *parse_mul(Parser *p) {
    AstNode *lhs = parse_unary(p);
    for (;;) {
        int line=p->cur.line,col=p->cur.col;
        int op = 0;
        if      (check(p,TOK_STAR))       op=TOK_STAR;
        else if (check(p,TOK_SLASH))      op=TOK_SLASH;
        else if (check(p,TOK_PERCENT))    op=TOK_PERCENT;
        else if (check(p,TOK_STAR_WRAP))  op=TOK_STAR_WRAP;
        else break;
        advance_tok(p);
        AstNode *rhs=parse_unary(p);
        AstNode *n=ast_node(EXPR_BINARY,line,col);
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs;
        lhs=n;
    }
    return lhs;
}

static AstNode *parse_add(Parser *p) {
    AstNode *lhs = parse_mul(p);
    for (;;) {
        int line=p->cur.line,col=p->cur.col; int op=0;
        if      (check(p,TOK_PLUS))       op=TOK_PLUS;
        else if (check(p,TOK_MINUS))      op=TOK_MINUS;
        else if (check(p,TOK_PLUS_WRAP))  op=TOK_PLUS_WRAP;
        else if (check(p,TOK_MINUS_WRAP)) op=TOK_MINUS_WRAP;
        else break;
        advance_tok(p);
        AstNode *rhs=parse_mul(p);
        AstNode *n=ast_node(EXPR_BINARY,line,col);
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs;
        lhs=n;
    }
    return lhs;
}

static AstNode *parse_shift(Parser *p) {
    AstNode *lhs = parse_add(p);
    for (;;) {
        int line=p->cur.line,col=p->cur.col; int op=0;
        if      (check(p,TOK_LSHIFT))  op=TOK_LSHIFT;
        else if (check(p,TOK_RSHIFT))  op=TOK_RSHIFT;
        else if (check(p,TOK_URSHIFT)) op=TOK_URSHIFT;
        else if (check(p,TOK_ROL))     op=TOK_ROL;
        else if (check(p,TOK_ROR))     op=TOK_ROR;
        else break;
        advance_tok(p);
        AstNode *rhs=parse_add(p);
        AstNode *n=ast_node(EXPR_BINARY,line,col);
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs;
        lhs=n;
    }
    return lhs;
}

static AstNode *parse_rel(Parser *p) {
    AstNode *lhs = parse_shift(p);
    for (;;) {
        int line=p->cur.line,col=p->cur.col; int op=0;
        if      (check(p,TOK_LT))  op=TOK_LT;
        else if (check(p,TOK_GT))  op=TOK_GT;
        else if (check(p,TOK_LEQ)) op=TOK_LEQ;
        else if (check(p,TOK_GEQ)) op=TOK_GEQ;
        else break;
        advance_tok(p);
        AstNode *rhs=parse_shift(p);
        AstNode *n=ast_node(EXPR_BINARY,line,col);
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs;
        lhs=n;
    }
    return lhs;
}

static AstNode *parse_eq(Parser *p) {
    AstNode *lhs = parse_rel(p);
    for (;;) {
        int line=p->cur.line,col=p->cur.col; int op=0;
        if      (check(p,TOK_EQ))  op=TOK_EQ;
        else if (check(p,TOK_NEQ)) op=TOK_NEQ;
        else break;
        advance_tok(p);
        AstNode *rhs=parse_rel(p);
        AstNode *n=ast_node(EXPR_BINARY,line,col);
        n->binary.op=op; n->binary.lhs=lhs; n->binary.rhs=rhs;
        lhs=n;
    }
    return lhs;
}

BINOP(parse_band, parse_eq, TOK_AMP,   0, TOK_AMP, 0)
BINOP(parse_bxor, parse_band, TOK_CARET, 0, TOK_CARET, 0)
BINOP(parse_bor,  parse_bxor, TOK_PIPE, 0, TOK_PIPE, 0)
BINOP(parse_and,  parse_bor,  TOK_AND,  0, TOK_AND, 0)
BINOP(parse_or,   parse_and,  TOK_OR,   0, TOK_OR, 0)

static AstNode *parse_assign(Parser *p) {
    AstNode *lhs = parse_or(p);
    int line=p->cur.line,col=p->cur.col;
    int op=0;
    switch (p->cur.type) {
    case TOK_ASSIGN:         op=TOK_ASSIGN; break;
    case TOK_PLUS_ASSIGN:    op=TOK_PLUS_ASSIGN; break;
    case TOK_MINUS_ASSIGN:   op=TOK_MINUS_ASSIGN; break;
    case TOK_STAR_ASSIGN:    op=TOK_STAR_ASSIGN; break;
    case TOK_SLASH_ASSIGN:   op=TOK_SLASH_ASSIGN; break;
    case TOK_PERCENT_ASSIGN: op=TOK_PERCENT_ASSIGN; break;
    case TOK_AMP_ASSIGN:     op=TOK_AMP_ASSIGN; break;
    case TOK_PIPE_ASSIGN:    op=TOK_PIPE_ASSIGN; break;
    case TOK_CARET_ASSIGN:   op=TOK_CARET_ASSIGN; break;
    case TOK_LSHIFT_ASSIGN:  op=TOK_LSHIFT_ASSIGN; break;
    case TOK_RSHIFT_ASSIGN:  op=TOK_RSHIFT_ASSIGN; break;
    default: return lhs;
    }
    advance_tok(p);
    AstNode *rhs = parse_assign(p);
    AstNode *n = ast_node(EXPR_ASSIGN, line, col);
    n->assign.op = op; n->assign.lhs = lhs; n->assign.rhs = rhs;
    return n;
}

static AstNode *parse_expr(Parser *p) { return parse_assign(p); }

/* ===== Statement parser ===== */

static AstList parse_attr_list(Parser *p) {
    AstList list = {0};
    while (check(p, TOK_AT)) {
        int line=p->cur.line,col=p->cur.col;
        advance_tok(p);
        char *name = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "attribute name");
        AstNode *n = ast_node(TOP_DIRECTIVE, line, col);
        n->directive.name = name;
        if (match_tok(p, TOK_LPAREN)) {
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ast_list_push(&n->directive.args, parse_expr(p));
                if (!match_tok(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RPAREN, "')'");
        }
        ast_list_push(&list, n);
        skip_newlines(p);
    }
    return list;
}

/* Accept any identifier-like token as a name */
static char *parse_name(Parser *p) {
    /* Allow keywords that are also valid identifiers in name positions */
    switch (p->cur.type) {
    case TOK_IDENT:
    case TOK_FLAGS:   case TOK_IN:   case TOK_MUT:
    case TOK_ALLOC:   case TOK_EXEC: case TOK_WRITE: case TOK_NOLOAD:
    case TOK_INTEL:   case TOK_GLOBAL: /* allow some */ {
        char *name = arena_strdup(p->cur.text);
        advance_tok(p);
        return name;
    }
    default:
        error(NULL, p->cur.line, p->cur.col, "expected variable name, got '%s'", p->cur.text);
        return arena_strdup("?");
    }
}

static AstNode *parse_var_decl(Parser *p, bool is_const) {
    int line=p->cur.line,col=p->cur.col;
    advance_tok(p); /* consume var/const */
    char *name = parse_name(p);
    AstNode *type_node = NULL;
    if (match_tok(p, TOK_COLON)) {
        type_node = parse_type(p);
    }
    AstNode *init = NULL;
    if (match_tok(p, TOK_ASSIGN)) {
        init = parse_expr(p);
    }
    AstNode *n = ast_node(is_const ? STMT_CONST : STMT_VAR, line, col);
    if (is_const) {
        n->const_decl.name = name;
        n->const_decl.type = type_node;
        n->const_decl.init = init;
    } else {
        n->var_decl.name = name;
        n->var_decl.type = type_node;
        n->var_decl.init = init;
    }
    return n;
}

static AstNode *parse_asm_block(Parser *p) {
    int line=p->cur.line,col=p->cur.col;
    advance_tok(p); /* asm */
    AstNode *n = ast_node(STMT_ASM, line, col);
    n->asm_block.intel = match_tok(p, TOK_INTEL);

    /* optional operand list */
    if (check(p, TOK_LBRACE) && false) { /* skip for now */ }

    /* parse asm strings block */
    expect(p, TOK_LBRACE, "'{'");
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        if (check(p, TOK_STRING_LIT)) {
            if (n->asm_block.str_count < 64)
                n->asm_block.strings[n->asm_block.str_count++] = arena_strdup(p->cur.text);
            advance_tok(p);
        }
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return n;
}

static AstNode *parse_stmt(Parser *p) {
    int line=p->cur.line,col=p->cur.col;
    skip_newlines(p);

    switch (p->cur.type) {
    case TOK_VAR: {
        AstNode *n = parse_var_decl(p, false);
        eat_eos(p);
        return n;
    }
    case TOK_CONST: {
        AstNode *n = parse_var_decl(p, true);
        eat_eos(p);
        return n;
    }
    case TOK_IF: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_IF, line, col);
        AstNode *cond = parse_expr(p);
        ast_list_push(&n->if_stmt.conds, cond);
        ast_list_push(&n->if_stmt.blocks, parse_block(p));
        while (check(p, TOK_ELSE)) {
            advance_tok(p);
            if (check(p, TOK_IF)) {
                advance_tok(p);
                ast_list_push(&n->if_stmt.conds, parse_expr(p));
                ast_list_push(&n->if_stmt.blocks, parse_block(p));
            } else {
                n->if_stmt.else_block = parse_block(p);
                break;
            }
        }
        return n;
    }
    case TOK_LOOP: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_LOOP, line, col);
        n->loop_stmt.body = parse_block(p);
        return n;
    }
    case TOK_WHILE: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_WHILE, line, col);
        n->while_stmt.cond = parse_expr(p);
        n->while_stmt.body = parse_block(p);
        return n;
    }
    case TOK_FOR: {
        advance_tok(p);
        AstNode *n;
        char *var = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "loop variable");
        if (check(p, TOK_IN)) {
            /* for_range: var in from .. to */
            advance_tok(p);
            AstNode *from = parse_expr(p);
            bool inc = false;
            if (check(p, TOK_DOTDOTEQ)) { inc = true; advance_tok(p); }
            else { expect(p, TOK_DOTDOT, "'..' or '..='"); }
            AstNode *to = parse_expr(p);
            AstNode *body = parse_block(p);
            n = ast_node(STMT_FOR_RANGE, line, col);
            n->for_range.var = var;
            n->for_range.from = from;
            n->for_range.to   = to;
            n->for_range.inclusive = inc;
            n->for_range.body = body;
        } else {
            /* for_c: var : type = init ; cond ; post block */
            expect(p, TOK_COLON, "':'");
            AstNode *ty = parse_type(p);
            expect(p, TOK_ASSIGN, "'='");
            AstNode *init = parse_expr(p);
            expect(p, TOK_SEMICOLON, "';'");
            AstNode *cond = parse_expr(p);
            expect(p, TOK_SEMICOLON, "';'");
            AstNode *post = parse_expr(p);
            AstNode *body = parse_block(p);
            n = ast_node(STMT_FOR_C, line, col);
            n->for_c.var  = var; n->for_c.type = ty;
            n->for_c.init = init; n->for_c.cond = cond;
            n->for_c.post = post; n->for_c.body = body;
        }
        return n;
    }
    case TOK_RETURN: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_RETURN, line, col);
        if (!check(p, TOK_SEMICOLON) && !check(p, TOK_NEWLINE) &&
            !check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            n->return_stmt.expr = parse_expr(p);
        }
        eat_eos(p);
        return n;
    }
    case TOK_GOTO: {
        advance_tok(p);
        char *target = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "label name");
        eat_eos(p);
        AstNode *n = ast_node(STMT_GOTO, line, col);
        n->goto_stmt.target = target;
        return n;
    }
    case TOK_BREAK: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_BREAK, line, col);
        if (check(p, TOK_IDENT)) {
            n->break_stmt.label = arena_strdup(p->cur.text);
            advance_tok(p);
        }
        eat_eos(p);
        return n;
    }
    case TOK_CONTINUE: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_CONTINUE, line, col);
        if (check(p, TOK_IDENT)) {
            n->continue_stmt.label = arena_strdup(p->cur.text);
            advance_tok(p);
        }
        eat_eos(p);
        return n;
    }
    case TOK_ASM: {
        return parse_asm_block(p);
    }
    case TOK_LBRACE: {
        return parse_block(p);
    }
    /* label_stmt: ':' IDENT */
    case TOK_COLON: {
        advance_tok(p);
        AstNode *n = ast_node(STMT_LABEL, line, col);
        n->label_stmt.name = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "label name");
        return n;
    }
    /* Handle label-prefixed loop: IDENT ':' loop/while/for */
    case TOK_IDENT: {
        if (check2(p, TOK_COLON)) {
            char *lbl = arena_strdup(p->cur.text);
            advance_tok(p); advance_tok(p); /* consume IDENT and ':' */
            AstNode *inner = parse_stmt(p);
            /* attach label */
            if (inner->kind == STMT_LOOP)       inner->loop_stmt.label = lbl;
            else if (inner->kind == STMT_WHILE)  inner->while_stmt.label = lbl;
            else if (inner->kind == STMT_FOR_C)  inner->for_c.label = lbl;
            else if (inner->kind == STMT_FOR_RANGE) inner->for_range.label = lbl;
            return inner;
        }
        /* fall through to expr_stmt */
    }
    default: {
        AstNode *expr = parse_expr(p);
        eat_eos(p);
        AstNode *n = ast_node(STMT_EXPR, line, col);
        n->expr_stmt.expr = expr;
        return n;
    }
    }
}

static AstNode *parse_block(Parser *p) {
    int line=p->cur.line,col=p->cur.col;
    expect(p, TOK_LBRACE, "'{'");
    AstNode *n = ast_node(STMT_BLOCK, line, col);
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        ast_list_push(&n->block.stmts, parse_stmt(p));
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return n;
}

/* ===== Top-level parser ===== */

static AstNode *parse_fn_decl(Parser *p, AstList attrs) {
    int line=p->cur.line,col=p->cur.col;
    /* parse modifiers */
    int mods = 0;
    while (check(p,TOK_GLOBAL)||check(p,TOK_EXTERN)||check(p,TOK_INLINE)) {
        if (check(p,TOK_GLOBAL))  { mods|=1; advance_tok(p); }
        if (check(p,TOK_EXTERN))  { mods|=2; advance_tok(p); }
        if (check(p,TOK_INLINE))  { mods|=4; advance_tok(p); }
    }
    expect(p, TOK_FN, "'fn'");
    char *name = arena_strdup(p->cur.text);
    expect(p, TOK_IDENT, "function name");
    expect(p, TOK_LPAREN, "'('");
    AstNode *n = ast_node(TOP_FN, line, col);
    n->fn_decl.attrs = attrs;
    n->fn_decl.modifiers = mods;
    n->fn_decl.name = name;
    skip_newlines(p);
    /* params */
    while (!check(p,TOK_RPAREN) && !check(p,TOK_EOF)) {
        if (check(p,TOK_ELLIPSIS)) { n->fn_decl.variadic=true; advance_tok(p); break; }
        if (check(p,TOK_DOTDOT)) { n->fn_decl.variadic=true; advance_tok(p); break; } /* also accept .. */
        int pline=p->cur.line,pcol=p->cur.col;
        char *pname = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "parameter name");
        expect(p, TOK_COLON, "':'");
        AstNode *pty = parse_type(p);
        AstNode *param = ast_node(STMT_VAR, pline, pcol);
        param->var_decl.name = pname;
        param->var_decl.type = pty;
        ast_list_push(&n->fn_decl.params, param);
        skip_newlines(p);
        if (!match_tok(p, TOK_COMMA)) break;
        skip_newlines(p);
        /* After comma, check for variadic */
        if (check(p,TOK_ELLIPSIS)) { n->fn_decl.variadic=true; advance_tok(p); break; }
        if (check(p,TOK_DOTDOT))   { n->fn_decl.variadic=true; advance_tok(p); break; }
    }
    expect(p, TOK_RPAREN, "')'");
    expect(p, TOK_ARROW, "'->'");
    n->fn_decl.ret_type = parse_type(p);
    if (mods & 2) { /* extern: no body */
        eat_eos(p);
    } else {
        n->fn_decl.body = parse_block(p);
    }
    return n;
}

static AstNode *parse_struct_decl(Parser *p, AstList attrs) {
    int line=p->cur.line,col=p->cur.col;
    bool is_union = check(p, TOK_UNION);
    advance_tok(p);
    char *name = arena_strdup(p->cur.text);
    expect(p, TOK_IDENT, "struct name");
    int align_val = 0;
    if (check(p, TOK_ALIGN)) {
        advance_tok(p);
        expect(p, TOK_LPAREN, "'('");
        align_val = (int)p->cur.val.ival;
        expect(p, TOK_INT_LIT, "alignment");
        expect(p, TOK_RPAREN, "')'");
    }
    expect(p, TOK_LBRACE, "'{'");
    AstNode *n = ast_node(is_union ? TOP_UNION : TOP_STRUCT, line, col);
    if (is_union) { n->union_decl.name = name; }
    else          { n->struct_decl.name = name; n->struct_decl.align = align_val; n->struct_decl.attrs = attrs; }
    skip_newlines(p);
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        char *fname = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "field name");
        expect(p, TOK_COLON, "':'");
        AstNode *ftype = parse_type(p);
        if (is_union) { ast_list_push(&n->union_decl.field_names, ast_node(EXPR_IDENT,line,col)); n->union_decl.field_names.items[n->union_decl.field_names.count-1]->ident.name=fname; ast_list_push(&n->union_decl.field_types, ftype); }
        else          { ast_list_push(&n->struct_decl.field_names, ast_node(EXPR_IDENT,line,col)); n->struct_decl.field_names.items[n->struct_decl.field_names.count-1]->ident.name=fname; ast_list_push(&n->struct_decl.field_types, ftype); }
        skip_newlines(p);
        match_tok(p, TOK_COMMA);
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return n;
}

static AstNode *parse_enum_decl(Parser *p) {
    int line=p->cur.line,col=p->cur.col;
    advance_tok(p); /* enum */
    char *name = arena_strdup(p->cur.text);
    expect(p, TOK_IDENT, "enum name");
    expect(p, TOK_COLON, "':'");
    /* base int type */
    IntKind base = INT_I32;
    switch (p->cur.type) {
    case TOK_U8: base=INT_U8; break; case TOK_U16: base=INT_U16; break;
    case TOK_U32: base=INT_U32; break; case TOK_U64: base=INT_U64; break;
    case TOK_I8: base=INT_I8; break; case TOK_I16: base=INT_I16; break;
    case TOK_I32: base=INT_I32; break; case TOK_I64: base=INT_I64; break;
    default: break;
    }
    advance_tok(p);
    expect(p, TOK_LBRACE, "'{'");
    AstNode *n = ast_node(TOP_ENUM, line, col);
    n->enum_decl.name = name; n->enum_decl.base_type = base;
    skip_newlines(p);
    int64_t auto_val = 0;
    while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
        skip_newlines(p);
        if (check(p, TOK_RBRACE)) break;
        int idx = n->enum_decl.var_count;
        n->enum_decl.var_names[idx] = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "enum variant");
        if (match_tok(p, TOK_ASSIGN)) {
            n->enum_decl.var_values[idx] = p->cur.val.ival;
            n->enum_decl.var_has_val[idx] = true;
            auto_val = p->cur.val.ival + 1;
            expect(p, TOK_INT_LIT, "integer");
        } else {
            n->enum_decl.var_values[idx] = auto_val++;
        }
        n->enum_decl.var_count++;
        skip_newlines(p);
        if (!match_tok(p, TOK_COMMA)) break;
        skip_newlines(p);
    }
    expect(p, TOK_RBRACE, "'}'");
    return n;
}

static AstNode *parse_top_item(Parser *p) {
    skip_newlines(p);
    int line=p->cur.line,col=p->cur.col;

    /* attributes */
    AstList attrs = parse_attr_list(p);

    switch (p->cur.type) {
    case TOK_IMPORT: {
        advance_tok(p);
        char *path = arena_strdup(p->cur.text);
        expect(p, TOK_STRING_LIT, "string");
        char *alias = NULL;
        if (match_tok(p, TOK_AS)) {
            alias = arena_strdup(p->cur.text);
            expect(p, TOK_IDENT, "alias");
        }
        eat_eos(p);
        AstNode *n = ast_node(TOP_IMPORT, line, col);
        n->import_decl.path = path; n->import_decl.alias = alias;
        return n;
    }
    case TOK_SECTION: {
        advance_tok(p);
        char *name = arena_strdup(p->cur.text);
        expect(p, TOK_STRING_LIT, "section name");
        int flags = 0;
        if (match_tok(p, TOK_FLAGS)) {
            expect(p, TOK_LPAREN, "'('");
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                if (check(p,TOK_ALLOC))  { flags|=1; advance_tok(p); }
                else if(check(p,TOK_EXEC)){flags|=2; advance_tok(p);}
                else if(check(p,TOK_WRITE)){flags|=4;advance_tok(p);}
                else if(check(p,TOK_NOLOAD)){flags|=8;advance_tok(p);}
                if (!match_tok(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RPAREN, "')'");
        }
        expect(p, TOK_LBRACE, "'{'");
        AstNode *n = ast_node(TOP_SECTION, line, col);
        n->section.name = name; n->section.flags = flags;
        skip_newlines(p);
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            ast_list_push(&n->section.items, parse_top_item(p));
            skip_newlines(p);
        }
        expect(p, TOK_RBRACE, "'}'");
        return n;
    }
    case TOK_GLOBAL: case TOK_EXTERN: case TOK_INLINE: case TOK_FN:
        return parse_fn_decl(p, attrs);
    case TOK_VAR: {
        AstNode *n = parse_var_decl(p, false);
        n->kind = TOP_VAR; /* re-tag as top-level */
        eat_eos(p);
        return n;
    }
    case TOK_CONST: {
        AstNode *n = parse_var_decl(p, true);
        n->kind = TOP_CONST;
        eat_eos(p);
        return n;
    }
    case TOK_STRUCT: case TOK_UNION:
        return parse_struct_decl(p, attrs);
    case TOK_ENUM:
        return parse_enum_decl(p);
    case TOK_MACRO: {
        advance_tok(p);
        AstNode *n = ast_node(TOP_MACRO, line, col);
        n->macro_decl.name = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "macro name");
        /* skip macro for now — just parse body */
        if (check(p, TOK_ASSIGN)) { advance_tok(p); n->macro_decl.body = parse_expr(p); }
        else if (check(p, TOK_LBRACE)) { n->macro_decl.body = parse_block(p); }
        eat_eos(p);
        return n;
    }
    case TOK_AT: {
        AstNode *n = ast_node(TOP_DIRECTIVE, line, col);
        advance_tok(p);
        n->directive.name = arena_strdup(p->cur.text);
        expect(p, TOK_IDENT, "directive name");
        if (match_tok(p, TOK_LPAREN)) {
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                ast_list_push(&n->directive.args, parse_expr(p));
                if (!match_tok(p, TOK_COMMA)) break;
            }
            expect(p, TOK_RPAREN, "')'");
        }
        eat_eos(p);
        return n;
    }
    case TOK_EOF:
        return NULL;
    default:
        error(p->filename, line, col, "unexpected token '%s' at top level", p->cur.text ? p->cur.text : "?");
        p->error_count++;
        advance_tok(p);
        return ast_node(TOP_DIRECTIVE, line, col); /* dummy */
    }
}

AstNode *parser_parse(Parser *p) {
    AstNode *prog = ast_node(NODE_PROGRAM, 1, 1);
    skip_newlines(p);
    while (!check(p, TOK_EOF)) {
        AstNode *item = parse_top_item(p);
        if (item) ast_list_push(&prog->program.items, item);
        skip_newlines(p);
    }
    return prog;
}
