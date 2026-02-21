#include "compiler.h"
#include <unistd.h>

static void usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [options] <input.src>\n"
        "Options:\n"
        "  -o <file>    output file (default: a.s)\n"
        "  -dump-ast    dump AST to stdout and exit\n"
        "  -c           compile to object file via assembler\n"
        "  -h           show this help\n",
        argv0);
    exit(1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *input   = NULL;
    const char *output  = "a.s";
    bool dump_ast = false;
    bool compile  = false;

    for (int i=1; i<argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
        } else if (strcmp(argv[i], "-o") == 0 && i+1 < argc) {
            output = argv[++i];
        } else if (strcmp(argv[i], "-dump-ast") == 0) {
            dump_ast = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            compile = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
        } else {
            if (input) { fprintf(stderr, "multiple inputs not supported\n"); exit(1); }
            input = argv[i];
        }
    }

    if (!input) usage(argv[0]);

    char *src = read_file(input);

    /* Parse */
    Parser *p = parser_new(src, input);
    AstNode *prog = parser_parse(p);

    if (p->error_count > 0) {
        fprintf(stderr, "%d error(s), aborting.\n", p->error_count);
        exit(1);
    }

    if (dump_ast) {
        ast_dump(prog, 0);
        exit(0);
    }

    /* Codegen */
    FILE *out;
    if (compile) {
        /* Write to temp .s file, then assemble */
        out = tmpfile();
        if (!out) { perror("tmpfile"); exit(1); }
    } else {
        if (strcmp(output, "-") == 0) {
            out = stdout;
        } else {
            out = fopen(output, "w");
            if (!out) { perror(output); exit(1); }
        }
    }

    Codegen *cg = codegen_new(out);
    codegen_program(cg, prog);

    if (compile) {
        /* Rewind and pipe to assembler */
        fseek(out, 0, SEEK_SET);
        /* Determine obj output name */
        char obj_name[512];
        if (strcmp(output, "a.s") == 0) {
            snprintf(obj_name, sizeof(obj_name), "a.out");
        } else {
            snprintf(obj_name, sizeof(obj_name), "%s", output);
        }
        /* Read temp asm */
        fseek(out, 0, SEEK_END);
        long asm_len = ftell(out);
        fseek(out, 0, SEEK_SET);
        char *asm_buf = malloc(asm_len + 1);
        fread(asm_buf, 1, asm_len, out);
        asm_buf[asm_len] = 0;
        fclose(out);

        /* Write to a .s temp file */
        char tmp_s[64];
        snprintf(tmp_s, sizeof(tmp_s), "/tmp/cmp_%d.s", (int)getpid());
        FILE *tf = fopen(tmp_s, "w");
        fwrite(asm_buf, 1, asm_len, tf);
        fclose(tf);
        free(asm_buf);

        /* Invoke GCC to assemble and link */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "gcc %s -o %s -no-pie 2>&1", tmp_s, obj_name);
        fprintf(stderr, "  [cc] %s\n", cmd);
        int ret = system(cmd);
        unlink(tmp_s);
        if (ret != 0) { fprintf(stderr, "assembler/linker failed.\n"); exit(1); }
    } else if (out != stdout) {
        fclose(out);
    }

    free(src);
    arena_reset();
    return 0;
}
