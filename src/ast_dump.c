#include "compiler.h"

static void indent(int n) { for(int i=0;i<n;i++) printf("  "); }

static void dump_type(AstNode *n, int ind) {
    if (!n) { indent(ind); printf("<null-type>\n"); return; }
    switch(n->kind) {
    case TY_INT:   indent(ind); printf("int(%s)\n", inttype_name(n->ty_int.kind)); break;
    case TY_FLOAT: indent(ind); printf("float(%s)\n", n->ty_float.kind==FLOAT_F32?"f32":"f64"); break;
    case TY_BOOL:  indent(ind); printf("bool\n"); break;
    case TY_VOID:  indent(ind); printf("void\n"); break;
    case TY_BYTE:  indent(ind); printf("byte\n"); break;
    case TY_PTR:   indent(ind); printf("*\n"); dump_type(n->ty_ptr.base, ind+1); break;
    case TY_MUT_PTR: indent(ind); printf("*mut\n"); dump_type(n->ty_ptr.base, ind+1); break;
    case TY_ARRAY: indent(ind); printf("[]\n"); dump_type(n->ty_array.base, ind+1); break;
    case TY_NAMED: indent(ind); printf("named(%s)\n", n->ty_named.name); break;
    case TY_FN:    indent(ind); printf("fn-type\n"); break;
    default:       indent(ind); printf("type(%d)\n", n->kind); break;
    }
}

void ast_dump(AstNode *node, int ind) {
    if (!node) { indent(ind); printf("<null>\n"); return; }
    switch(node->kind) {
    case NODE_PROGRAM:
        indent(ind); printf("program (%d items)\n", node->program.items.count);
        for(int i=0;i<node->program.items.count;i++)
            ast_dump(node->program.items.items[i], ind+1);
        break;
    case TOP_FN:
        indent(ind); printf("fn %s (mods=%d)\n", node->fn_decl.name, node->fn_decl.modifiers);
        indent(ind+1); printf("params: %d\n", node->fn_decl.params.count);
        for(int i=0;i<node->fn_decl.params.count;i++) {
            AstNode *p = node->fn_decl.params.items[i];
            indent(ind+2); printf("%s: ", p->var_decl.name);
            dump_type(p->var_decl.type, 0);
        }
        indent(ind+1); printf("ret: "); dump_type(node->fn_decl.ret_type, 0);
        if (node->fn_decl.body) ast_dump(node->fn_decl.body, ind+1);
        break;
    case TOP_VAR: case STMT_VAR:
        indent(ind); printf("var %s\n", node->var_decl.name);
        if (node->var_decl.type) dump_type(node->var_decl.type, ind+1);
        if (node->var_decl.init) ast_dump(node->var_decl.init, ind+1);
        break;
    case TOP_CONST: case STMT_CONST:
        indent(ind); printf("const %s\n", node->const_decl.name);
        if (node->const_decl.init) ast_dump(node->const_decl.init, ind+1);
        break;
    case TOP_STRUCT:
        indent(ind); printf("struct %s (fields=%d)\n", node->struct_decl.name, node->struct_decl.field_names.count);
        break;
    case TOP_ENUM:
        indent(ind); printf("enum %s (%d variants)\n", node->enum_decl.name, node->enum_decl.var_count);
        break;
    case TOP_IMPORT:
        indent(ind); printf("import \"%s\"\n", node->import_decl.path);
        break;
    case STMT_BLOCK:
        indent(ind); printf("block (%d stmts)\n", node->block.stmts.count);
        for(int i=0;i<node->block.stmts.count;i++)
            ast_dump(node->block.stmts.items[i], ind+1);
        break;
    case STMT_RETURN:
        indent(ind); printf("return\n");
        if (node->return_stmt.expr) ast_dump(node->return_stmt.expr, ind+1);
        break;
    case STMT_IF:
        indent(ind); printf("if (%d branches)\n", node->if_stmt.conds.count);
        for(int i=0;i<node->if_stmt.conds.count;i++) {
            indent(ind+1); printf("cond:\n"); ast_dump(node->if_stmt.conds.items[i], ind+2);
            indent(ind+1); printf("then:\n"); ast_dump(node->if_stmt.blocks.items[i], ind+2);
        }
        if (node->if_stmt.else_block) { indent(ind+1); printf("else:\n"); ast_dump(node->if_stmt.else_block, ind+2); }
        break;
    case STMT_WHILE:
        indent(ind); printf("while\n");
        ast_dump(node->while_stmt.cond, ind+1);
        ast_dump(node->while_stmt.body, ind+1);
        break;
    case STMT_LOOP:
        indent(ind); printf("loop\n");
        ast_dump(node->loop_stmt.body, ind+1);
        break;
    case STMT_FOR_C:
        indent(ind); printf("for_c (%s)\n", node->for_c.var);
        break;
    case STMT_FOR_RANGE:
        indent(ind); printf("for_range (%s in ..)\n", node->for_range.var);
        break;
    case STMT_EXPR:
        indent(ind); printf("expr-stmt\n");
        ast_dump(node->expr_stmt.expr, ind+1);
        break;
    case STMT_GOTO:
        indent(ind); printf("goto %s\n", node->goto_stmt.target);
        break;
    case STMT_LABEL:
        indent(ind); printf("label :%s\n", node->label_stmt.name);
        break;
    case STMT_BREAK:
        indent(ind); printf("break\n");
        break;
    case STMT_CONTINUE:
        indent(ind); printf("continue\n");
        break;
    case STMT_ASM:
        indent(ind); printf("asm (%d strings)\n", node->asm_block.str_count);
        break;
    case EXPR_INT:
        indent(ind); printf("int(%lld)\n", (long long)node->int_lit.val);
        break;
    case EXPR_FLOAT:
        indent(ind); printf("float(%g)\n", node->float_lit.val);
        break;
    case EXPR_BOOL:
        indent(ind); printf("bool(%s)\n", node->bool_lit.val?"true":"false");
        break;
    case EXPR_NULL:
        indent(ind); printf("null\n");
        break;
    case EXPR_CHAR:
        indent(ind); printf("char('%c')\n", node->char_lit.codepoint);
        break;
    case EXPR_STRING:
        indent(ind); printf("string(\"%s\")\n", node->str_lit.val);
        break;
    case EXPR_IDENT:
        indent(ind); printf("ident(%s)\n", node->ident.name);
        break;
    case EXPR_UNARY:
        indent(ind); printf("unary(op=%d)\n", node->unary.op);
        ast_dump(node->unary.operand, ind+1);
        break;
    case EXPR_BINARY:
        indent(ind); printf("binary(op=%d)\n", node->binary.op);
        ast_dump(node->binary.lhs, ind+1);
        ast_dump(node->binary.rhs, ind+1);
        break;
    case EXPR_ASSIGN:
        indent(ind); printf("assign(op=%d)\n", node->assign.op);
        ast_dump(node->assign.lhs, ind+1);
        ast_dump(node->assign.rhs, ind+1);
        break;
    case EXPR_CALL:
        indent(ind); printf("call (%d args)\n", node->call.args.count);
        ast_dump(node->call.callee, ind+1);
        for(int i=0;i<node->call.args.count;i++) ast_dump(node->call.args.items[i], ind+1);
        break;
    case EXPR_FIELD:
        indent(ind); printf("field(.%s)\n", node->field.field);
        ast_dump(node->field.base, ind+1);
        break;
    case EXPR_ARROW:
        indent(ind); printf("arrow(->%s)\n", node->field.field);
        ast_dump(node->field.base, ind+1);
        break;
    case EXPR_INDEX:
        indent(ind); printf("index\n");
        ast_dump(node->index.base, ind+1);
        ast_dump(node->index.index, ind+1);
        break;
    case EXPR_CAST:
        indent(ind); printf("cast\n");
        dump_type(node->cast.type, ind+1);
        ast_dump(node->cast.expr, ind+1);
        break;
    case EXPR_SIZEOF_T:
        indent(ind); printf("sizeof(type)\n");
        dump_type(node->sizeof_t.type, ind+1);
        break;
    case EXPR_SIZEOF_E:
        indent(ind); printf("sizeof(expr)\n");
        ast_dump(node->sizeof_e.expr, ind+1);
        break;
    case EXPR_STRUCT_INIT:
        indent(ind); printf("struct_init (%d fields)\n", node->struct_init.fields.count);
        break;
    case EXPR_ARRAY_INIT:
        indent(ind); printf("array_init (%d elems)\n", node->array_init.elems.count);
        break;
    default:
        indent(ind); printf("node(%d)\n", node->kind);
        break;
    }
}
