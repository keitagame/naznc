#include "compiler.h"

/* Keyword table */
static const struct { const char *word; TokenType tok; } KEYWORDS[] = {
    {"import",   TOK_IMPORT},   {"as",      TOK_AS},
    {"section",  TOK_SECTION},  {"flags",   TOK_FLAGS},
    {"fn",       TOK_FN},       {"global",  TOK_GLOBAL},
    {"extern",   TOK_EXTERN},   {"inline",  TOK_INLINE},
    {"var",      TOK_VAR},      {"const",   TOK_CONST},
    {"struct",   TOK_STRUCT},   {"union",   TOK_UNION},
    {"enum",     TOK_ENUM},     {"macro",   TOK_MACRO},
    {"align",    TOK_ALIGN},
    {"alloc",    TOK_ALLOC},    {"exec",    TOK_EXEC},
    {"write",    TOK_WRITE},    {"noload",  TOK_NOLOAD},
    {"if",       TOK_IF},       {"else",    TOK_ELSE},
    {"loop",     TOK_LOOP},     {"while",   TOK_WHILE},
    {"for",      TOK_FOR},      {"in",      TOK_IN},
    {"goto",     TOK_GOTO},     {"break",   TOK_BREAK},
    {"continue", TOK_CONTINUE}, {"return",  TOK_RETURN},
    {"asm",      TOK_ASM},      {"intel",   TOK_INTEL},
    {"cast",     TOK_CAST},     {"sizeof",  TOK_SIZEOF},
    {"alignof",  TOK_ALIGNOF},  {"offsetof",TOK_OFFSETOF},
    {"true",     TOK_TRUE},     {"false",   TOK_FALSE},
    {"null",     TOK_NULL},
    {"u8",       TOK_U8},       {"u16",     TOK_U16},
    {"u32",      TOK_U32},      {"u64",     TOK_U64},
    {"i8",       TOK_I8},       {"i16",     TOK_I16},
    {"i32",      TOK_I32},      {"i64",     TOK_I64},
    {"f32",      TOK_F32},      {"f64",     TOK_F64},
    {"bool",     TOK_BOOL},     {"void",    TOK_VOID},
    {"byte",     TOK_BYTE},     {"usize",   TOK_USIZE},
    {"isize",    TOK_ISIZE},    {"mut",     TOK_MUT},
    {"rol",      TOK_ROL},      {"ror",     TOK_ROR},
    {NULL, 0}
};

Lexer *lexer_new(const char *src, const char *filename) {
    Lexer *l = arena_alloc(sizeof(Lexer));
    l->src      = src;
    l->pos      = 0;
    l->line     = 1;
    l->col      = 1;
    l->filename = filename;
    return l;
}

void lexer_free(Lexer *l) { (void)l; }

static char peek_ch(Lexer *l) {
    return l->src[l->pos];
}
static char peek2(Lexer *l) {
    return l->src[l->pos + 1];
}
static char advance(Lexer *l) {
    char c = l->src[l->pos++];
    if (c == '\n') { l->line++; l->col = 1; }
    else            { l->col++; }
    return c;
}
static bool match(Lexer *l, char c) {
    if (l->src[l->pos] == c) { advance(l); return true; }
    return false;
}

static void skip_whitespace_and_comments(Lexer *l) {
    for (;;) {
        char c = peek_ch(l);
        if (c == ' ' || c == '\t' || c == '\r') { advance(l); continue; }
        /* line comment */
        if (c == '/' && peek2(l) == '/') {
            while (peek_ch(l) && peek_ch(l) != '\n') advance(l);
            continue;
        }
        /* block comment */
        if (c == '/' && peek2(l) == '*') {
            advance(l); advance(l);
            while (peek_ch(l)) {
                if (peek_ch(l) == '*' && l->src[l->pos+1] == '/') {
                    advance(l); advance(l); break;
                }
                advance(l);
            }
            continue;
        }
        break;
    }
}

static Token make_tok(TokenType t, const char *text, int line, int col) {
    Token tok = {0};
    tok.type = t;
    tok.text = arena_strdup(text);
    tok.line = line;
    tok.col  = col;
    return tok;
}

static uint64_t parse_digits(Lexer *l, int base, char *buf, int *blen) {
    uint64_t val = 0;
    while (1) {
        char c = peek_ch(l);
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = 10 + c - 'a';
        else if (c >= 'A' && c <= 'F') digit = 10 + c - 'A';
        else if (c == '_') { advance(l); continue; }
        else break;
        if (digit >= base) break;
        val = val * base + digit;
        if (buf && *blen < 63) buf[(*blen)++] = c;
        advance(l);
    }
    return val;
}

static int parse_escape(Lexer *l) {
    char c = advance(l);
    switch (c) {
    case 'n':  return '\n';
    case 't':  return '\t';
    case 'r':  return '\r';
    case '\\': return '\\';
    case '\'': return '\'';
    case '"':  return '"';
    case '0':  return 0;
    case 'x': {
        char buf[3] = {0};
        int blen = 0;
        return (int)parse_digits(l, 16, buf, &blen);
    }
    default: return c;
    }
}

Token lexer_next(Lexer *l) {
    skip_whitespace_and_comments(l);

    int line = l->line, col = l->col;
    char c = peek_ch(l);

    if (c == '\0') return make_tok(TOK_EOF, "", line, col);

    /* Newline as statement terminator */
    if (c == '\n') { advance(l); return make_tok(TOK_NEWLINE, "\n", line, col); }

    /* String literal */
    if (c == '"' || ((c == 'b' || c == 'c') && peek2(l) == '"')) {
        bool is_bytes = false, is_c = false;
        if (c == 'b') { is_bytes = true; advance(l); }
        else if (c == 'c') { is_c = true; advance(l); }
        advance(l); /* opening " */
        char buf[4096]; int blen = 0;
        while (peek_ch(l) && peek_ch(l) != '"') {
            char cc = peek_ch(l);
            if (cc == '\\') { advance(l); buf[blen++] = (char)parse_escape(l); }
            else { buf[blen++] = cc; advance(l); }
        }
        buf[blen] = 0;
        if (peek_ch(l) == '"') advance(l);
        Token tok = make_tok(TOK_STRING_LIT, buf, line, col);
        /* encode prefix in suffix field */
        if (is_bytes) tok.suffix[0] = 'b';
        else if (is_c) tok.suffix[0] = 'c';
        return tok;
    }

    /* Char literal */
    if (c == '\'') {
        advance(l);
        int cp;
        if (peek_ch(l) == '\\') { advance(l); cp = parse_escape(l); }
        else { cp = (unsigned char)peek_ch(l); advance(l); }
        if (peek_ch(l) == '\'') advance(l);
        Token tok = make_tok(TOK_CHAR_LIT, "'?'", line, col);
        tok.val.ival = cp;
        return tok;
    }

    /* Number literal */
    if ((c >= '0' && c <= '9') ||
        (c == '0' && (peek2(l) == 'x' || peek2(l) == 'b' || peek2(l) == 'o'))) {
        char buf[64] = {0}; int blen = 0;
        int base = 10;
        uint64_t val = 0;
        bool is_float = false;
        if (c == '0' && (peek2(l) == 'x' || peek2(l) == 'X')) {
            advance(l); advance(l); base = 16;
        } else if (c == '0' && (peek2(l) == 'b' || peek2(l) == 'B')) {
            advance(l); advance(l); base = 2;
        } else if (c == '0' && (peek2(l) == 'o' || peek2(l) == 'O')) {
            advance(l); advance(l); base = 8;
        }
        val = parse_digits(l, base, buf, &blen);
        /* float? */
        if (base == 10 && peek_ch(l) == '.') {
            is_float = true;
            buf[blen++] = '.'; advance(l);
            parse_digits(l, 10, buf + blen, &blen);
        }
        if (base == 10 && (peek_ch(l) == 'e' || peek_ch(l) == 'E')) {
            is_float = true;
            buf[blen++] = advance(l);
            if (peek_ch(l) == '+' || peek_ch(l) == '-') buf[blen++] = advance(l);
            parse_digits(l, 10, buf + blen, &blen);
        }
        /* suffix */
        char suffix[8] = {0}; int slen = 0;
        while ((peek_ch(l) >= 'a' && peek_ch(l) <= 'z') ||
               (peek_ch(l) >= 'A' && peek_ch(l) <= 'Z') ||
               (peek_ch(l) >= '0' && peek_ch(l) <= '9')) {
            suffix[slen++] = advance(l);
        }
        if (is_float) {
            Token tok = make_tok(TOK_FLOAT_LIT, buf, line, col);
            tok.val.fval = atof(buf);
            memcpy(tok.suffix, suffix, sizeof(suffix));
            return tok;
        } else {
            char tbuf[32]; snprintf(tbuf, sizeof(tbuf), "%lld", (long long)val);
            Token tok = make_tok(TOK_INT_LIT, tbuf, line, col);
            tok.val.ival = (int64_t)val;
            memcpy(tok.suffix, suffix, sizeof(suffix));
            return tok;
        }
    }

    /* Identifier / keyword */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        char buf[256]; int blen = 0;
        while (1) {
            char cc = peek_ch(l);
            if ((cc >= 'a' && cc <= 'z') || (cc >= 'A' && cc <= 'Z') ||
                (cc >= '0' && cc <= '9') || cc == '_') {
                buf[blen++] = advance(l);
            } else break;
        }
        buf[blen] = 0;
        /* keyword lookup */
        for (int i = 0; KEYWORDS[i].word; i++) {
            if (strcmp(buf, KEYWORDS[i].word) == 0)
                return make_tok(KEYWORDS[i].tok, buf, line, col);
        }
        return make_tok(TOK_IDENT, buf, line, col);
    }

    /* Punctuation */
    advance(l);
    switch (c) {
    case '+':
        if (match(l,'=')) return make_tok(TOK_PLUS_ASSIGN, "+=", line, col);
        if (match(l,'%')) {
            if (match(l,'=')) return make_tok(TOK_PLUS_ASSIGN, "+%=", line, col); /* reuse */
            return make_tok(TOK_PLUS_WRAP, "+%", line, col);
        }
        return make_tok(TOK_PLUS, "+", line, col);
    case '-':
        if (match(l,'>')) return make_tok(TOK_ARROW, "->", line, col);
        if (match(l,'=')) return make_tok(TOK_MINUS_ASSIGN, "-=", line, col);
        if (match(l,'%')) {
            if (match(l,'=')) return make_tok(TOK_MINUS_ASSIGN, "-%=", line, col);
            return make_tok(TOK_MINUS_WRAP, "-%", line, col);
        }
        return make_tok(TOK_MINUS, "-", line, col);
    case '*':
        if (match(l,'=')) return make_tok(TOK_STAR_ASSIGN, "*=", line, col);
        if (match(l,'%')) {
            if (match(l,'=')) return make_tok(TOK_STAR_ASSIGN, "*%=", line, col);
            return make_tok(TOK_STAR_WRAP, "*%", line, col);
        }
        return make_tok(TOK_STAR, "*", line, col);
    case '/':
        if (match(l,'=')) return make_tok(TOK_SLASH_ASSIGN, "/=", line, col);
        return make_tok(TOK_SLASH, "/", line, col);
    case '%':
        if (match(l,'=')) return make_tok(TOK_PERCENT_ASSIGN, "%=", line, col);
        return make_tok(TOK_PERCENT, "%", line, col);
    case '&':
        if (match(l,'&')) return make_tok(TOK_AND, "&&", line, col);
        if (match(l,'=')) return make_tok(TOK_AMP_ASSIGN, "&=", line, col);
        return make_tok(TOK_AMP, "&", line, col);
    case '|':
        if (match(l,'|')) return make_tok(TOK_OR, "||", line, col);
        if (match(l,'=')) return make_tok(TOK_PIPE_ASSIGN, "|=", line, col);
        return make_tok(TOK_PIPE, "|", line, col);
    case '^':
        if (match(l,'=')) return make_tok(TOK_CARET_ASSIGN, "^=", line, col);
        return make_tok(TOK_CARET, "^", line, col);
    case '~': return make_tok(TOK_TILDE, "~", line, col);
    case '!':
        if (match(l,'=')) return make_tok(TOK_NEQ, "!=", line, col);
        return make_tok(TOK_BANG, "!", line, col);
    case '<':
        if (match(l,'<')) {
            if (match(l,'=')) return make_tok(TOK_LSHIFT_ASSIGN, "<<=", line, col);
            return make_tok(TOK_LSHIFT, "<<", line, col);
        }
        if (match(l,'=')) return make_tok(TOK_LEQ, "<=", line, col);
        return make_tok(TOK_LT, "<", line, col);
    case '>':
        if (match(l,'>')) {
            if (peek_ch(l) == '>') { advance(l); return make_tok(TOK_URSHIFT, ">>>", line, col); }
            if (match(l,'=')) return make_tok(TOK_RSHIFT_ASSIGN, ">>=", line, col);
            return make_tok(TOK_RSHIFT, ">>", line, col);
        }
        if (match(l,'=')) return make_tok(TOK_GEQ, ">=", line, col);
        return make_tok(TOK_GT, ">", line, col);
    case '=':
        if (match(l,'=')) return make_tok(TOK_EQ, "==", line, col);
        return make_tok(TOK_ASSIGN, "=", line, col);
    case '.':
        if (peek_ch(l) == '.') {
            advance(l);
            if (peek_ch(l) == '.') { advance(l); return make_tok(TOK_ELLIPSIS, "...", line, col); }
            if (match(l,'=')) return make_tok(TOK_DOTDOTEQ, "..=", line, col);
            return make_tok(TOK_DOTDOT, "..", line, col);
        }
        return make_tok(TOK_DOT, ".", line, col);
    case '(': return make_tok(TOK_LPAREN,   "(", line, col);
    case ')': return make_tok(TOK_RPAREN,   ")", line, col);
    case '{': return make_tok(TOK_LBRACE,   "{", line, col);
    case '}': return make_tok(TOK_RBRACE,   "}", line, col);
    case '[': return make_tok(TOK_LBRACKET, "[", line, col);
    case ']': return make_tok(TOK_RBRACKET, "]", line, col);
    case ',': return make_tok(TOK_COMMA,    ",", line, col);
    case ';': return make_tok(TOK_SEMICOLON,";", line, col);
    case ':':
        if (match(l,':')) return make_tok(TOK_COLONCOLON, "::", line, col);
        return make_tok(TOK_COLON, ":", line, col);
    case '@': return make_tok(TOK_AT,       "@", line, col);
    default: {
        char buf[4] = {c, 0};
        return make_tok(TOK_EOF, buf, line, col); /* unknown */
    }
    }
}
