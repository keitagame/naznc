#include "compiler.h"

/* ===== Simple arena allocator ===== */
#define ARENA_SIZE (64 * 1024 * 1024)  /* 64 MB */
static char  arena_buf[ARENA_SIZE];
static size_t arena_pos = 0;

void *arena_alloc(size_t n) {
    n = (n + 7) & ~7; /* align to 8 bytes */
    if (arena_pos + n > ARENA_SIZE) {
        fprintf(stderr, "fatal: out of arena memory\n");
        exit(1);
    }
    void *p = arena_buf + arena_pos;
    arena_pos += n;
    memset(p, 0, n);
    return p;
}

char *arena_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = arena_alloc(n);
    memcpy(d, s, n);
    return d;
}

void arena_reset(void) {
    arena_pos = 0;
}

/* ===== AST helpers ===== */
AstNode *ast_node(NodeKind kind, int line, int col) {
    AstNode *n = arena_alloc(sizeof(AstNode));
    n->kind = kind;
    n->line = line;
    n->col  = col;
    return n;
}

void ast_list_push(AstList *list, AstNode *node) {
    if (list->count >= list->cap) {
        int new_cap = list->cap ? list->cap * 2 : 4;
        AstNode **new_items = arena_alloc(sizeof(AstNode*) * new_cap);
        if (list->items)
            memcpy(new_items, list->items, sizeof(AstNode*) * list->count);
        list->items = new_items;
        list->cap   = new_cap;
    }
    list->items[list->count++] = node;
}

/* ===== Error reporting ===== */
void error(const char *file, int line, int col, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", file ? file : "?", line, col);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

void warning(const char *file, int line, int col, const char *fmt, ...) {
    fprintf(stderr, "%s:%d:%d: warning: ", file ? file : "?", line, col);
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fprintf(stderr, "\n");
}

/* ===== Type helpers ===== */
const char *inttype_name(IntKind k) {
    switch (k) {
    case INT_U8:  return "u8";
    case INT_U16: return "u16";
    case INT_U32: return "u32";
    case INT_U64: return "u64";
    case INT_I8:  return "i8";
    case INT_I16: return "i16";
    case INT_I32: return "i32";
    case INT_I64: return "i64";
    default: return "?";
    }
}

bool inttype_signed(IntKind k) { return k >= INT_I8; }

int inttype_bytes(IntKind k) {
    switch (k) {
    case INT_U8: case INT_I8:   return 1;
    case INT_U16: case INT_I16: return 2;
    case INT_U32: case INT_I32: return 4;
    case INT_U64: case INT_I64: return 8;
    default: return 8;
    }
}

/* ===== Scope ===== */
#define SCOPE_BUCKETS 64

Scope *scope_new(Scope *parent) {
    Scope *s = arena_alloc(sizeof(Scope));
    s->table  = arena_alloc(sizeof(Symbol*) * SCOPE_BUCKETS);
    s->size   = SCOPE_BUCKETS;
    s->parent = parent;
    return s;
}

static unsigned hash_str(const char *s) {
    unsigned h = 5381;
    while (*s) h = h * 33 ^ (unsigned char)*s++;
    return h;
}

Symbol *scope_lookup(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        unsigned h = hash_str(name) % s->size;
        for (Symbol *sym = s->table[h]; sym; sym = sym->next) {
            if (strcmp(sym->name, name) == 0) return sym;
        }
    }
    return NULL;
}

Symbol *scope_define(Scope *s, const char *name, SymKind kind, AstNode *decl) {
    unsigned h = hash_str(name) % s->size;
    /* check for duplicate in current scope only */
    for (Symbol *sym = s->table[h]; sym; sym = sym->next) {
        if (strcmp(sym->name, name) == 0) return NULL; /* duplicate */
    }
    Symbol *sym = arena_alloc(sizeof(Symbol));
    sym->name = arena_strdup(name);
    sym->kind = kind;
    sym->decl = decl;
    sym->next = s->table[h];
    s->table[h] = sym;
    return sym;
}
