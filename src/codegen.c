#include "compiler.h"

/* ===================================================================
   x86-64 System V ABI code generator
   Outputs GNU AT&T syntax assembly for use with GAS (as) or clang/gcc
   =================================================================== */

#define MAX_STRINGS 4096
static char *string_pool[MAX_STRINGS];
static int   string_count = 0;

static int new_label(Codegen *cg) { return cg->label_count++; }

static void emit(Codegen *cg, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fputc('\n', cg->out);
}

static void emit_label(Codegen *cg, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(cg->out, fmt, ap);
    va_end(ap);
    fputs(":\n", cg->out);
}

/* ---- Register allocation (simple: all in rax for scalar) ---- */
/* Calling convention: rdi, rsi, rdx, rcx, r8, r9, then stack */
static const char *ARG_REGS[] = {"rdi","rsi","rdx","rcx","r8","r9"};
#define NARG_REGS 6

/* Size-appropriate move suffix */
static const char *mov_suffix(int bytes) {
    switch(bytes) { case 1: return "b"; case 2: return "w"; case 4: return "l"; default: return "q"; }
}
static const char *reg_for_size(const char *r64, int bytes) {
    /* simple: only handle rax */
    if (strcmp(r64,"rax")==0) {
        switch(bytes) { case 1: return "al"; case 2: return "ax"; case 4: return "eax"; default: return "rax"; }
    }
    if (strcmp(r64,"rdi")==0) {
        switch(bytes) { case 1: return "dil"; case 2: return "di"; case 4: return "edi"; default: return "rdi"; }
    }
    if (strcmp(r64,"rsi")==0) {
        switch(bytes) { case 1: return "sil"; case 2: return "si"; case 4: return "esi"; default: return "rsi"; }
    }
    return r64;
}

/* ---- Type sizing ---- */
static int type_size(AstNode *ty) {
    if (!ty) return 8;
    switch(ty->kind) {
    case TY_INT:   return inttype_bytes(ty->ty_int.kind);
    case TY_FLOAT: return ty->ty_float.kind==FLOAT_F32?4:8;
    case TY_BOOL:  return 1;
    case TY_BYTE:  return 1;
    case TY_VOID:  return 0;
    case TY_PTR: case TY_MUT_PTR: return 8;
    case TY_USIZE: case TY_ISIZE: return 8;
    default:       return 8;
    }
}

static bool type_signed(AstNode *ty) {
    if (!ty) return true;
    if (ty->kind == TY_INT) return inttype_signed(ty->ty_int.kind);
    return true;
}

/* ---- Forward declarations ---- */
static void cg_expr(Codegen *cg, AstNode *n);
static void cg_stmt(Codegen *cg, AstNode *n);
static void cg_lvalue(Codegen *cg, AstNode *n); /* leaves address in rax */

Codegen *codegen_new(FILE *out) {
    Codegen *cg = calloc(1, sizeof(Codegen));
    cg->out = out;
    cg->scope = scope_new(NULL);
    return cg;
}

/* ---- Scope helpers ---- */
static void push_scope(Codegen *cg) {
    cg->scope = scope_new(cg->scope);
}
static void pop_scope(Codegen *cg) {
    cg->scope = cg->scope->parent;
}

/* allocate a local variable on the stack, return offset */
static int alloc_local(Codegen *cg, int size) {
    size = (size < 8) ? 8 : (size+7)&~7;
    cg->stack_offset += size;
    return -cg->stack_offset;
}

/* ---- Expression codegen ---- */
/* Result always in rax (or xmm0 for float) */

static void cg_lvalue(Codegen *cg, AstNode *n) {
    /* puts address of lvalue in rax */
    switch(n->kind) {
    case EXPR_IDENT: {
        Symbol *sym = scope_lookup(cg->scope, n->ident.name);
        if (!sym) {
            error(NULL, n->line, n->col, "undefined: %s", n->ident.name);
            emit(cg, "  xorq %%rax, %%rax");
            return;
        }
        if (sym->is_global) {
            emit(cg, "  leaq %s(%%rip), %%rax", sym->name);
        } else {
            emit(cg, "  leaq %d(%%rbp), %%rax", sym->offset);
        }
        break;
    }
    case EXPR_UNARY:
        if (n->unary.op == TOK_STAR) {
            /* *ptr -> value of ptr is address */
            cg_expr(cg, n->unary.operand);
            /* rax now holds the pointer value */
        } else {
            error(NULL, n->line, n->col, "not an lvalue");
        }
        break;
    case EXPR_FIELD: {
        /* &base.field */
        cg_lvalue(cg, n->field.base);
        /* TODO: add offset based on struct layout */
        /* For now just leave rax as is (offset=0 placeholder) */
        break;
    }
    case EXPR_ARROW: {
        cg_expr(cg, n->field.base); /* ptr in rax */
        /* TODO: add field offset */
        break;
    }
    case EXPR_INDEX: {
        /* &base[index] */
        cg_expr(cg, n->index.index);
        emit(cg, "  pushq %%rax");
        cg_lvalue(cg, n->index.base);
        emit(cg, "  popq %%rcx");
        emit(cg, "  leaq (%%rax,%%rcx,8), %%rax"); /* assumes 8-byte elements TODO */
        break;
    }
    default:
        error(NULL, n->line, n->col, "not an lvalue (kind=%d)", n->kind);
        emit(cg, "  xorq %%rax, %%rax");
        break;
    }
}

static void cg_expr(Codegen *cg, AstNode *n) {
    if (!n) { emit(cg, "  xorq %%rax, %%rax"); return; }
    switch(n->kind) {
    case EXPR_INT:
        emit(cg, "  movq $%lld, %%rax", (long long)n->int_lit.val);
        break;
    case EXPR_FLOAT:
        /* float: load via .rodata constant */
        {
            int lbl = new_label(cg);
            if (n->float_lit.kind == FLOAT_F64) {
                emit(cg, "  movsd .Lflt%d(%%rip), %%xmm0", lbl);
                /* emit the constant later — we use a hack: emit inline data */
                /* Actually we'll emit as a comment and put in data section */
                fprintf(cg->out, ".section .rodata\n");
                fprintf(cg->out, ".Lflt%d: .double %g\n", lbl, n->float_lit.val);
                fprintf(cg->out, ".text\n");
            } else {
                emit(cg, "  movss .Lflt%d(%%rip), %%xmm0", lbl);
                fprintf(cg->out, ".section .rodata\n");
                fprintf(cg->out, ".Lflt%d: .float %g\n", lbl, n->float_lit.val);
                fprintf(cg->out, ".text\n");
            }
        }
        break;
    case EXPR_BOOL:
        emit(cg, "  movq $%d, %%rax", n->bool_lit.val ? 1 : 0);
        break;
    case EXPR_NULL:
        emit(cg, "  xorq %%rax, %%rax");
        break;
    case EXPR_CHAR:
        emit(cg, "  movq $%d, %%rax", n->char_lit.codepoint);
        break;
    case EXPR_STRING: {
        int idx = string_count++;
        if (idx < MAX_STRINGS) string_pool[idx] = n->str_lit.val;
        emit(cg, "  leaq .Lstr%d(%%rip), %%rax", idx);
        break;
    }
    case EXPR_IDENT: {
        Symbol *sym = scope_lookup(cg->scope, n->ident.name);
        if (!sym) {
            error(NULL, n->line, n->col, "undefined: %s", n->ident.name);
            emit(cg, "  xorq %%rax, %%rax");
            return;
        }
        if (sym->is_global) {
            if (sym->kind == SYM_CONST) {
                /* Could be an enum variant (.equ) or const — use immediate */
                emit(cg, "  movq $%s, %%rax", sym->name);
            } else {
                emit(cg, "  movq %s(%%rip), %%rax", sym->name);
            }
        } else {
            int sz = sym->type_node ? type_size(sym->type_node) : 8;
            const char *suf = mov_suffix(sz);
            const char *r   = reg_for_size("rax", sz);
            if (sz < 8) emit(cg, "  xorq %%rax, %%rax");
            if (type_signed(sym->type_node) && sz < 8) {
                if (sz == 1) emit(cg, "  movsbq %d(%%rbp), %%rax", sym->offset);
                else if (sz == 2) emit(cg, "  movswq %d(%%rbp), %%rax", sym->offset);
                else emit(cg, "  movslq %d(%%rbp), %%rax", sym->offset);
            } else {
                emit(cg, "  mov%s %d(%%rbp), %%%s", suf, sym->offset, r);
                if (sz < 8) emit(cg, "  movzx%s %%%s, %%rax", suf, r); /* zero extend for unsigned */
            }
        }
        break;
    }
    case EXPR_UNARY: {
        if (n->unary.op == TOK_AMP || n->unary.op == (int)'M') {
            cg_lvalue(cg, n->unary.operand);
            break;
        }
        cg_expr(cg, n->unary.operand);
        switch(n->unary.op) {
        case TOK_MINUS: emit(cg, "  negq %%rax"); break;
        case TOK_TILDE: emit(cg, "  notq %%rax"); break;
        case TOK_BANG:
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  sete %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_STAR: /* dereference */
            emit(cg, "  movq (%%rax), %%rax");
            break;
        }
        break;
    }
    case EXPR_BINARY: {
        /* Evaluate lhs, push, eval rhs, pop */
        int op = n->binary.op;
        /* Short-circuit for && and || */
        if (op == TOK_AND) {
            int lbl_false = new_label(cg);
            int lbl_end   = new_label(cg);
            cg_expr(cg, n->binary.lhs);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  jz .L%d", lbl_false);
            cg_expr(cg, n->binary.rhs);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  setnz %%al");
            emit(cg, "  movzbq %%al, %%rax");
            emit(cg, "  jmp .L%d", lbl_end);
            emit_label(cg, ".L%d", lbl_false);
            emit(cg, "  xorq %%rax, %%rax");
            emit_label(cg, ".L%d", lbl_end);
            break;
        }
        if (op == TOK_OR) {
            int lbl_true = new_label(cg);
            int lbl_end  = new_label(cg);
            cg_expr(cg, n->binary.lhs);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  jnz .L%d", lbl_true);
            cg_expr(cg, n->binary.rhs);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  setnz %%al");
            emit(cg, "  movzbq %%al, %%rax");
            emit(cg, "  jmp .L%d", lbl_end);
            emit_label(cg, ".L%d", lbl_true);
            emit(cg, "  movq $1, %%rax");
            emit_label(cg, ".L%d", lbl_end);
            break;
        }
        cg_expr(cg, n->binary.lhs);
        emit(cg, "  pushq %%rax");
        cg_expr(cg, n->binary.rhs);
        emit(cg, "  movq %%rax, %%rcx"); /* rcx = rhs */
        emit(cg, "  popq %%rax");         /* rax = lhs */
        switch(op) {
        case TOK_PLUS: case TOK_PLUS_WRAP:
            emit(cg, "  addq %%rcx, %%rax"); break;
        case TOK_MINUS: case TOK_MINUS_WRAP:
            emit(cg, "  subq %%rcx, %%rax"); break;
        case TOK_STAR: case TOK_STAR_WRAP:
            emit(cg, "  imulq %%rcx, %%rax"); break;
        case TOK_SLASH:
            emit(cg, "  cqto");
            emit(cg, "  idivq %%rcx");
            break;
        case TOK_PERCENT:
            emit(cg, "  cqto");
            emit(cg, "  idivq %%rcx");
            emit(cg, "  movq %%rdx, %%rax");
            break;
        case TOK_AMP:    emit(cg, "  andq %%rcx, %%rax"); break;
        case TOK_PIPE:   emit(cg, "  orq  %%rcx, %%rax"); break;
        case TOK_CARET:  emit(cg, "  xorq %%rcx, %%rax"); break;
        case TOK_LSHIFT: emit(cg, "  shlq %%cl, %%rax"); break;
        case TOK_RSHIFT: emit(cg, "  sarq %%cl, %%rax"); break;
        case TOK_URSHIFT:emit(cg, "  shrq %%cl, %%rax"); break;
        case TOK_ROL:    emit(cg, "  rolq %%cl, %%rax"); break;
        case TOK_ROR:    emit(cg, "  rorq %%cl, %%rax"); break;
        case TOK_EQ:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  sete %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_NEQ:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  setne %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_LT:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  setl %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_GT:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  setg %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_LEQ:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  setle %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        case TOK_GEQ:
            emit(cg, "  cmpq %%rcx, %%rax");
            emit(cg, "  setge %%al");
            emit(cg, "  movzbq %%al, %%rax");
            break;
        default:
            emit(cg, "  /* unhandled binop %d */", op);
        }
        break;
    }
    case EXPR_ASSIGN: {
        int op = n->assign.op;
        if (op != TOK_ASSIGN) {
            /* compound: load lhs, do op, store */
            cg_expr(cg, n->assign.lhs);
            emit(cg, "  pushq %%rax");
            cg_expr(cg, n->assign.rhs);
            emit(cg, "  movq %%rax, %%rcx");
            emit(cg, "  popq %%rax");
            switch(op) {
            case TOK_PLUS_ASSIGN:  emit(cg, "  addq %%rcx, %%rax"); break;
            case TOK_MINUS_ASSIGN: emit(cg, "  subq %%rcx, %%rax"); break;
            case TOK_STAR_ASSIGN:  emit(cg, "  imulq %%rcx, %%rax"); break;
            case TOK_SLASH_ASSIGN: emit(cg, "  cqto"); emit(cg, "  idivq %%rcx"); break;
            case TOK_PERCENT_ASSIGN: emit(cg, "  cqto"); emit(cg, "  idivq %%rcx"); emit(cg, "  movq %%rdx, %%rax"); break;
            case TOK_AMP_ASSIGN:   emit(cg, "  andq %%rcx, %%rax"); break;
            case TOK_PIPE_ASSIGN:  emit(cg, "  orq %%rcx, %%rax"); break;
            case TOK_CARET_ASSIGN: emit(cg, "  xorq %%rcx, %%rax"); break;
            case TOK_LSHIFT_ASSIGN: emit(cg, "  shlq %%cl, %%rax"); break;
            case TOK_RSHIFT_ASSIGN: emit(cg, "  sarq %%cl, %%rax"); break;
            default: break;
            }
            emit(cg, "  pushq %%rax");
            cg_lvalue(cg, n->assign.lhs);
            emit(cg, "  popq %%rcx");
            emit(cg, "  movq %%rcx, (%%rax)");
            emit(cg, "  movq %%rcx, %%rax");
        } else {
            cg_expr(cg, n->assign.rhs);
            emit(cg, "  pushq %%rax");
            cg_lvalue(cg, n->assign.lhs);
            emit(cg, "  popq %%rcx");
            emit(cg, "  movq %%rcx, (%%rax)");
            emit(cg, "  movq %%rcx, %%rax");
        }
        break;
    }
    case EXPR_CALL: {
        /* Evaluate args right-to-left and put in registers */
        int argc = n->call.args.count;
        /* Save args on stack first (eval left-to-right) */
        for (int i = 0; i < argc; i++) {
            cg_expr(cg, n->call.args.items[i]);
            emit(cg, "  pushq %%rax");
        }
        /* Pop into arg registers in reverse */
        for (int i = argc-1; i >= 0; i--) {
            if (i < NARG_REGS) {
                emit(cg, "  popq %%%s", ARG_REGS[i]);
            } else {
                /* extra args go on stack (already there) */
            }
        }
        /* Align stack */
        if (argc > NARG_REGS) {
            int extra = argc - NARG_REGS;
            int misalign = (extra % 2) * 8;
            if (misalign) emit(cg, "  subq $8, %%rsp");
        }
        /* Call */
        if (n->call.callee->kind == EXPR_IDENT) {
            emit(cg, "  xorb %%al, %%al"); /* vararg: 0 xmm regs */
            emit(cg, "  callq %s", n->call.callee->ident.name);
        } else {
            cg_expr(cg, n->call.callee);
            emit(cg, "  xorb %%al, %%al");
            emit(cg, "  callq *%%rax");
        }
        /* Restore if extra stack args */
        if (argc > NARG_REGS) {
            int extra = argc - NARG_REGS;
            int adj = extra * 8 + ((extra%2)*8);
            emit(cg, "  addq $%d, %%rsp", adj);
        }
        break;
    }
    case EXPR_FIELD: {
        /* load address, then dereference field */
        cg_lvalue(cg, n);
        emit(cg, "  movq (%%rax), %%rax");
        break;
    }
    case EXPR_ARROW: {
        cg_expr(cg, n->field.base);
        /* TODO: real field offset lookup */
        emit(cg, "  movq (%%rax), %%rax");
        break;
    }
    case EXPR_INDEX: {
        cg_expr(cg, n->index.index);
        emit(cg, "  pushq %%rax");
        cg_expr(cg, n->index.base); /* base address / pointer */
        emit(cg, "  popq %%rcx");
        emit(cg, "  movq (%%rax,%%rcx,8), %%rax"); /* 8-byte elem TODO */
        break;
    }
    case EXPR_CAST: {
        cg_expr(cg, n->cast.expr);
        /* For integer casts, truncate/extend */
        int dst_sz = type_size(n->cast.type);
        bool dst_sign = type_signed(n->cast.type);
        if (dst_sz == 1) {
            emit(cg, "  movzbq %%al, %%rax");
        } else if (dst_sz == 2) {
            emit(cg, "  movzwq %%ax, %%rax");
        } else if (dst_sz == 4) {
            emit(cg, "  movl %%eax, %%eax");
        }
        /* 8-byte: nothing */
        (void)dst_sign;
        break;
    }
    case EXPR_SIZEOF_T:
        emit(cg, "  movq $%d, %%rax", type_size(n->sizeof_t.type));
        break;
    case EXPR_SIZEOF_E:
        /* Can't fully determine size without type inference */
        emit(cg, "  movq $8, %%rax  /* sizeof(expr) approx */");
        break;
    case EXPR_ALIGNOF:
        emit(cg, "  movq $%d, %%rax", type_size(n->alignof_.type)); /* approx */
        break;
    case EXPR_STRUCT_INIT:
        /* Allocate temp on stack and fill fields */
        emit(cg, "  /* struct_init not fully implemented */");
        emit(cg, "  leaq -8(%%rsp), %%rax");
        break;
    case EXPR_ARRAY_INIT:
    case EXPR_ARRAY_REPEAT:
        emit(cg, "  /* array_init not fully implemented */");
        emit(cg, "  xorq %%rax, %%rax");
        break;
    case EXPR_DIRECTIVE:
        /* @builtin directives */
        emit(cg, "  /* directive @%s */", n->directive_expr.name);
        emit(cg, "  xorq %%rax, %%rax");
        break;
    default:
        emit(cg, "  /* unhandled expr kind %d */", n->kind);
        emit(cg, "  xorq %%rax, %%rax");
        break;
    }
}

/* ---- Statement codegen ---- */
static void cg_stmt(Codegen *cg, AstNode *n) {
    if (!n) return;
    emit(cg, "  /* [%s:%d] */", n->kind <= STMT_ASM ? "stmt" : "node", n->line);
    switch(n->kind) {
    case STMT_BLOCK: {
        push_scope(cg);
        for (int i=0; i<n->block.stmts.count; i++)
            cg_stmt(cg, n->block.stmts.items[i]);
        pop_scope(cg);
        break;
    }
    case STMT_VAR: case TOP_VAR: {
        int sz = n->var_decl.type ? type_size(n->var_decl.type) : 8;
        int off = alloc_local(cg, sz);
        Symbol *sym = scope_define(cg->scope, n->var_decl.name, SYM_LOCAL, n);
        if (sym) { sym->offset = off; sym->type_node = n->var_decl.type; }
        if (n->var_decl.init) {
            cg_expr(cg, n->var_decl.init);
            const char *suf = mov_suffix(sz < 8 ? sz : 8);
            const char *r   = reg_for_size("rax", sz < 8 ? sz : 8);
            emit(cg, "  mov%s %%%s, %d(%%rbp)", suf, r, off);
        }
        break;
    }
    case STMT_CONST: case TOP_CONST: {
        int off = alloc_local(cg, 8);
        Symbol *sym = scope_define(cg->scope, n->const_decl.name, SYM_CONST, n);
        if (sym) { sym->offset = off; sym->type_node = n->const_decl.type; }
        if (n->const_decl.init) {
            cg_expr(cg, n->const_decl.init);
            emit(cg, "  movq %%rax, %d(%%rbp)", off);
        }
        break;
    }
    case STMT_EXPR:
        cg_expr(cg, n->expr_stmt.expr);
        break;
    case STMT_RETURN:
        if (n->return_stmt.expr) {
            cg_expr(cg, n->return_stmt.expr);
        } else {
            emit(cg, "  xorq %%rax, %%rax");
        }
        emit(cg, "  movq %%rbp, %%rsp");
        emit(cg, "  popq %%rbp");
        emit(cg, "  retq");
        break;
    case STMT_IF: {
        int n_branches = n->if_stmt.conds.count;
        int lbl_end = new_label(cg);
        int lbl_next = -1;
        for (int i=0; i<n_branches; i++) {
            if (lbl_next >= 0) emit_label(cg, ".L%d", lbl_next);
            lbl_next = new_label(cg);
            cg_expr(cg, n->if_stmt.conds.items[i]);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  jz .L%d", lbl_next);
            cg_stmt(cg, n->if_stmt.blocks.items[i]);
            emit(cg, "  jmp .L%d", lbl_end);
        }
        emit_label(cg, ".L%d", lbl_next);
        if (n->if_stmt.else_block) cg_stmt(cg, n->if_stmt.else_block);
        emit_label(cg, ".L%d", lbl_end);
        break;
    }
    case STMT_WHILE: {
        int lbl_top = new_label(cg);
        int lbl_end = new_label(cg);
        const char *lbl = n->while_stmt.label;
        int d = cg->loop_depth++;
        snprintf(cg->break_labels[d], 64, ".L%d", lbl_end);
        snprintf(cg->cont_labels[d],  64, ".L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_top);
        cg_expr(cg, n->while_stmt.cond);
        emit(cg, "  testq %%rax, %%rax");
        emit(cg, "  jz .L%d", lbl_end);
        cg_stmt(cg, n->while_stmt.body);
        emit(cg, "  jmp .L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_end);
        cg->loop_depth--;
        (void)lbl;
        break;
    }
    case STMT_LOOP: {
        int lbl_top = new_label(cg);
        int lbl_end = new_label(cg);
        int d = cg->loop_depth++;
        snprintf(cg->break_labels[d], 64, ".L%d", lbl_end);
        snprintf(cg->cont_labels[d],  64, ".L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_top);
        cg_stmt(cg, n->loop_stmt.body);
        emit(cg, "  jmp .L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_end);
        cg->loop_depth--;
        break;
    }
    case STMT_FOR_C: {
        push_scope(cg);
        int sz = n->for_c.type ? type_size(n->for_c.type) : 8;
        int off = alloc_local(cg, sz);
        Symbol *sym = scope_define(cg->scope, n->for_c.var, SYM_LOCAL, n);
        if (sym) { sym->offset = off; sym->type_node = n->for_c.type; }
        /* init */
        if (n->for_c.init) {
            cg_expr(cg, n->for_c.init);
            emit(cg, "  movq %%rax, %d(%%rbp)", off);
        }
        int lbl_top  = new_label(cg);
        int lbl_post = new_label(cg);
        int lbl_end  = new_label(cg);
        int d = cg->loop_depth++;
        snprintf(cg->break_labels[d], 64, ".L%d", lbl_end);
        snprintf(cg->cont_labels[d],  64, ".L%d", lbl_post);
        emit_label(cg, ".L%d", lbl_top);
        if (n->for_c.cond) {
            cg_expr(cg, n->for_c.cond);
            emit(cg, "  testq %%rax, %%rax");
            emit(cg, "  jz .L%d", lbl_end);
        }
        cg_stmt(cg, n->for_c.body);
        emit_label(cg, ".L%d", lbl_post);
        if (n->for_c.post) cg_expr(cg, n->for_c.post);
        emit(cg, "  jmp .L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_end);
        cg->loop_depth--;
        pop_scope(cg);
        break;
    }
    case STMT_FOR_RANGE: {
        push_scope(cg);
        int off = alloc_local(cg, 8);
        Symbol *sym = scope_define(cg->scope, n->for_range.var, SYM_LOCAL, n);
        if (sym) { sym->offset = off; }
        /* init = from */
        cg_expr(cg, n->for_range.from);
        emit(cg, "  movq %%rax, %d(%%rbp)", off);
        int lbl_top = new_label(cg);
        int lbl_end = new_label(cg);
        int d = cg->loop_depth++;
        snprintf(cg->break_labels[d], 64, ".L%d", lbl_end);
        snprintf(cg->cont_labels[d],  64, ".L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_top);
        emit(cg, "  movq %d(%%rbp), %%rax", off);
        emit(cg, "  pushq %%rax");
        cg_expr(cg, n->for_range.to);
        emit(cg, "  popq %%rcx"); /* rcx = var, rax = limit */
        if (n->for_range.inclusive)
            emit(cg, "  cmpq %%rax, %%rcx"); /* var <= limit */
        else
            emit(cg, "  cmpq %%rax, %%rcx"); /* var < limit */
        emit(cg, "  jge .L%d", lbl_end);
        cg_stmt(cg, n->for_range.body);
        /* increment */
        emit(cg, "  incq %d(%%rbp)", off);
        emit(cg, "  jmp .L%d", lbl_top);
        emit_label(cg, ".L%d", lbl_end);
        cg->loop_depth--;
        pop_scope(cg);
        break;
    }
    case STMT_BREAK:
        if (cg->loop_depth > 0)
            emit(cg, "  jmp %s", cg->break_labels[cg->loop_depth-1]);
        else
            emit(cg, "  /* break outside loop */");
        break;
    case STMT_CONTINUE:
        if (cg->loop_depth > 0)
            emit(cg, "  jmp %s", cg->cont_labels[cg->loop_depth-1]);
        else
            emit(cg, "  /* continue outside loop */");
        break;
    case STMT_GOTO:
        emit(cg, "  jmp .Llbl_%s", n->goto_stmt.target);
        break;
    case STMT_LABEL:
        emit_label(cg, ".Llbl_%s", n->label_stmt.name);
        break;
    case STMT_ASM: {
        emit(cg, "  /* inline asm */");
        if (n->asm_block.intel) {
            emit(cg, "  .intel_syntax noprefix");
        }
        for (int i=0; i<n->asm_block.str_count; i++) {
            fprintf(cg->out, "  %s\n", n->asm_block.strings[i]);
        }
        if (n->asm_block.intel) {
            emit(cg, "  .att_syntax prefix");
        }
        break;
    }
    default:
        emit(cg, "  /* unhandled stmt kind %d */", n->kind);
        break;
    }
}

/* ---- Function codegen ---- */
static void cg_fn(Codegen *cg, AstNode *n) {
    const char *name = n->fn_decl.name;
    int mods = n->fn_decl.modifiers;

    /* Declare in global scope */
    Symbol *sym = scope_define(cg->scope, name, SYM_FN, n);
    if (sym) sym->is_global = true;

    if (mods & 2) { /* extern: just declare */
        return;
    }

    /* Emit function label */
    if (mods & 1) { /* global */
        emit(cg, "  .globl %s", name);
    }
    emit_label(cg, "%s", name);

    /* Prologue */
    emit(cg, "  pushq %%rbp");
    emit(cg, "  movq %%rsp, %%rbp");

    /* Reserve space for locals (we'll fixup after analyzing) */
    /* Two-pass approach: first estimate, then emit placeholder */
    /* For simplicity, reserve 256 bytes and hope for the best */
    emit(cg, "  subq $256, %%rsp");

    /* Enter new scope */
    cg->stack_offset = 0;
    push_scope(cg);
    cg->current_fn = name;

    /* Bind parameters */
    for (int i=0; i<n->fn_decl.params.count; i++) {
        AstNode *param = n->fn_decl.params.items[i];
        int sz = type_size(param->var_decl.type);
        int off = alloc_local(cg, sz);
        Symbol *psym = scope_define(cg->scope, param->var_decl.name, SYM_PARAM, param);
        if (psym) { psym->offset = off; psym->type_node = param->var_decl.type; }
        /* store argument register to stack slot */
        if (i < NARG_REGS) {
            const char *suf = mov_suffix(8);
            emit(cg, "  mov%s %%%s, %d(%%rbp)", suf, ARG_REGS[i], off);
        } else {
            /* passed on stack: positive rbp offset */
            int stack_arg_off = 16 + (i - NARG_REGS) * 8;
            emit(cg, "  movq %d(%%rbp), %%rax", stack_arg_off);
            emit(cg, "  movq %%rax, %d(%%rbp)", off);
        }
    }

    /* Emit body */
    if (n->fn_decl.body) cg_stmt(cg, n->fn_decl.body);

    pop_scope(cg);

    /* Epilogue (fallthrough return) */
    emit(cg, "  xorq %%rax, %%rax");
    emit(cg, "  movq %%rbp, %%rsp");
    emit(cg, "  popq %%rbp");
    emit(cg, "  retq");
    emit(cg, "");
}

/* ---- Global variable codegen ---- */
static void cg_global_var(Codegen *cg, AstNode *n) {
    const char *name = n->var_decl.name ? n->var_decl.name : n->const_decl.name;
    bool is_const = (n->kind == TOP_CONST);
    int sz = n->var_decl.type ? type_size(n->var_decl.type) : 8;

    Symbol *sym = scope_define(cg->scope, name, is_const ? SYM_CONST : SYM_VAR, n);
    if (sym) { sym->is_global = true; sym->name = arena_strdup(name); sym->type_node = n->var_decl.type; }

    if (is_const) {
        fprintf(cg->out, ".section .rodata\n");
    } else {
        fprintf(cg->out, ".data\n");
    }
    fprintf(cg->out, "  .globl %s\n", name);
    fprintf(cg->out, "%s:\n", name);

    /* Check for initializer */
    AstNode *init = n->var_decl.init;
    if (init && init->kind == EXPR_INT) {
        switch(sz) {
        case 1: fprintf(cg->out, "  .byte %lld\n", (long long)init->int_lit.val); break;
        case 2: fprintf(cg->out, "  .short %lld\n", (long long)init->int_lit.val); break;
        case 4: fprintf(cg->out, "  .long %lld\n", (long long)init->int_lit.val); break;
        default: fprintf(cg->out, "  .quad %lld\n", (long long)init->int_lit.val); break;
        }
    } else if (init && init->kind == EXPR_STRING) {
        fprintf(cg->out, "  .string \"%s\"\n", init->str_lit.val);
    } else {
        fprintf(cg->out, "  .zero %d\n", sz < 1 ? 8 : sz);
    }
    fprintf(cg->out, ".text\n");
}

/* ---- Struct declaration (generate size/offset info) ---- */
static void cg_struct(Codegen *cg, AstNode *n) {
    (void)cg; (void)n;
    /* Struct declarations don't emit code; they're recorded for type sizing */
    /* TODO: register struct layout in a table for offsetof and field access */
}

/* ---- Enum declaration ---- */
static void cg_enum(Codegen *cg, AstNode *n) {
    /* Emit each variant as a constant in the .rodata section or just declare */
    /* For now, register each variant in scope as a constant */
    for (int i=0; i<n->enum_decl.var_count; i++) {
        Symbol *sym = scope_define(cg->scope, n->enum_decl.var_names[i], SYM_CONST, n);
        if (sym) { sym->is_global = true; }
        /* Emit as equate in assembly */
        fprintf(cg->out, ".equ %s, %lld\n", n->enum_decl.var_names[i], (long long)n->enum_decl.var_values[i]);
    }
}

/* ---- Program codegen ---- */
void codegen_program(Codegen *cg, AstNode *prog) {
    fprintf(cg->out, "  .file \"program\"\n");
    fprintf(cg->out, ".text\n");

    /* First pass: register extern fns */
    for (int i=0; i<prog->program.items.count; i++) {
        AstNode *item = prog->program.items.items[i];
        if (item->kind == TOP_FN && (item->fn_decl.modifiers & 2)) {
            Symbol *sym = scope_define(cg->scope, item->fn_decl.name, SYM_FN, item);
            if (sym) sym->is_global = true;
        }
    }

    /* Second pass: emit */
    for (int i=0; i<prog->program.items.count; i++) {
        AstNode *item = prog->program.items.items[i];
        switch(item->kind) {
        case TOP_FN:      cg_fn(cg, item); break;
        case TOP_VAR:     cg_global_var(cg, item); break;
        case TOP_CONST:   cg_global_var(cg, item); break;
        case TOP_STRUCT:  cg_struct(cg, item); break;
        case TOP_UNION:   cg_struct(cg, item); break;
        case TOP_ENUM:    cg_enum(cg, item); break;
        case TOP_IMPORT:  /* handled by linker */ break;
        case TOP_SECTION: /* TODO: section directives */
            for (int j=0; j<item->section.items.count; j++)
                /* Recurse into section items as if top-level */
                codegen_program(cg, &(AstNode){.kind=NODE_PROGRAM, .program.items=item->section.items});
            break;
        default: break;
        }
    }

    /* Emit string pool */
    if (string_count > 0) {
        fprintf(cg->out, ".section .rodata\n");
        for (int i=0; i<string_count; i++) {
            fprintf(cg->out, ".Lstr%d:\n", i);
            fprintf(cg->out, "  .string \"");
            const char *s = string_pool[i];
            for (; *s; s++) {
                if (*s == '\n')       fprintf(cg->out, "\\n");
                else if (*s == '\t')  fprintf(cg->out, "\\t");
                else if (*s == '"')   fprintf(cg->out, "\\\"");
                else if (*s == '\\')  fprintf(cg->out, "\\\\");
                else                  fputc(*s, cg->out);
            }
            fprintf(cg->out, "\"\n");
        }
        fprintf(cg->out, ".text\n");
    }
}
